/* * singbox_tray_with_converter.c
 * * Refactored version with:
 * 1. Process Crash Monitoring (MonitorThread)
 * 2. Stdout/Stderr Log Monitoring (LogMonitorThread)
 * 3. (NEW) Automatic Node Switching on connection errors (timeout, refused)
 * 4. Auto-Switch Cooldown (60s)
 * 5. Robust log buffer parsing
 * 6. (NEW) Log Viewer Window to display live sing-box output
 * 7. (NEW) Auto-fix duplicate tags on startup (silently)
 *
 * (Modification): Node switching now targets 'route.final'
 * (Modification): Add/Delete/Update nodes now syncs with '自动切换' selector
 */

#define UNICODE
#define _UNICODE

// 必须在包含任何 Windows 头文件之前定义
#define _WIN32_IE 0x0601
#define __STDC_WANT_LIB_EXT1__ 1
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wininet.h>
#include <commctrl.h>
#include <time.h> // 用于重启冷却

// 直接包含源文件以简化 TCC 编译过程
#include "cJSON.c"

// 为兼容旧版 SDK (如某些 MinGW 版本) 手动添加缺失的宏定义
#ifndef NIF_GUID
#define NIF_GUID 0x00000020
#endif

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

// 定义一个唯一的 GUID，仅用于程序单实例
// {BFD8A583-662A-4FE3-9784-FAB78A3386A3}
static const GUID APP_GUID = { 0xbfd8a583, 0x662a, 0x4fe3, { 0x97, 0x84, 0xfa, 0xb7, 0x8a, 0x33, 0x86, 0xa3 } };


#define WM_TRAY (WM_USER + 1)
#define WM_SINGBOX_CRASHED (WM_USER + 2)     // 消息：核心进程崩溃
#define WM_SINGBOX_RECONNECT (WM_USER + 3)   // 消息：日志检测到错误，请求自动切换
#define WM_LOG_UPDATE (WM_USER + 4)          // 消息：日志线程发送新的日志文本

#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003
#define ID_TRAY_OPEN_CONVERTER 1004
#define ID_TRAY_SETTINGS 1005
#define ID_TRAY_MANAGE_NODES 1006
#define ID_TRAY_SHOW_CONSOLE 1007 // 新增：显示日志菜单ID
#define ID_TRAY_NODE_BASE 2000

// 节点管理窗口控件ID
#define ID_NODEMGR_LISTBOX 3001
#define ID_NODEMGR_MODIFY_BTN 3002
#define ID_NODEMGR_DELETE_BTN 3003
#define ID_NODEMGR_INFO_LABEL 3004
#define ID_NODEMGR_CONTEXT_SELECT_ALL 3005
#define ID_NODEMGR_CONTEXT_DESELECT_ALL 3006
#define ID_NODEMGR_ADD_BTN 3007
#define ID_NODEMGR_CONTEXT_PIN_NODE 3008
#define ID_NODEMGR_CONTEXT_DEDUPLICATE 3009
#define ID_NODEMGR_CONTEXT_SORT_NODES 3010
// 移除了 ID_NODEMGR_CONTEXT_FIX_DUPLICATE_TAGS


// 修改节点对话框控件ID
#define ID_MODIFY_EDIT_CONTENT 4001
#define ID_MODIFY_OK_BTN 4002
#define ID_MODIFY_CANCEL_BTN 4003
#define ID_MODIFY_FORMAT_BTN 4004

// 添加节点对话框控件ID
#define ID_ADD_EDIT_CONTENT 5001
#define ID_ADD_OK_BTN 5002
#define ID_ADD_CANCEL_BTN 5003
#define ID_ADD_FORMAT_BTN 5004

// 日志查看器窗口控件ID
#define ID_LOGVIEWER_EDIT 6001

#define ID_GLOBAL_HOTKEY 9001
#define ID_HOTKEY_CTRL 101

#ifndef IDR_HTML_CONVERTER
#define IDR_HTML_CONVERTER 2
#endif
#ifndef RT_HTML
#define RT_HTML L"HTML"
#endif

// 全局变量
NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi = {0};
HFONT g_hFont = NULL; // 全局字体句柄

wchar_t** nodeTags = NULL;
int nodeCount = 0;
int nodeCapacity = 0;
wchar_t currentNode[64] = L"";
int httpPort = 0;

const wchar_t* REG_PATH_PROXY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

// 新增全局变量
BOOL g_isIconVisible = TRUE;
UINT g_hotkeyModifiers = 0;
UINT g_hotkeyVk = 0;
wchar_t g_iniFilePath[MAX_PATH] = {0};

// --- 重构：新增守护功能全局变量 ---
HANDLE hMonitorThread = NULL;           // 进程崩溃监控线程
HANDLE hLogMonitorThread = NULL;        // 进程日志监控线程
HANDLE hChildStd_OUT_Rd_Global = NULL;  // 核心进程的标准输出管道（读取端）
BOOL g_isExiting = FALSE;               // 标记是否为用户主动退出/切换

// --- 新增：日志窗口句柄 ---
HWND hLogViewerWnd = NULL; // 日志查看器窗口句柄
HFONT hLogFont = NULL;     // 日志窗口等宽字体
// --- 重构结束 ---

// 用于在窗口间传递数据的结构体
typedef struct {
    wchar_t oldTag[256];
    wchar_t newTag[256]; // 将在成功修改后被填充
    BOOL success;
} MODIFY_NODE_PARAMS;


// 函数声明
void ShowTrayTip(const wchar_t* title, const wchar_t* message);
void ShowError(const wchar_t* title, const wchar_t* message);
BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize);
void CleanupDynamicNodes();
BOOL IsWindows8OrGreater();
void LoadSettings();
void SaveSettings();
void ToggleTrayIconVisibility();
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OpenSettingsWindow();
BOOL ParseTags();
int GetHttpInboundPort();
void StartSingBox();
void SwitchNode(const wchar_t* tag);
void SetSystemProxy(BOOL enable);
BOOL IsSystemProxyEnabled();
void SafeReplaceOutbound(const wchar_t* newTag);
void UpdateMenu();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void StopSingBox();
void SetAutorun(BOOL enable);
BOOL IsAutorunEnabled();
void OpenConverterHtmlFromResource();
char* ConvertLfToCrlf(const char* input);
void CreateDefaultConfig();

// 节点管理函数声明
void OpenNodeManagerWindow();
LRESULT CALLBACK NodeManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ModifyNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK AddNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL DeleteNodeByTag(const wchar_t* tagToDelete);
char* GetNodeContentByTag(const wchar_t* tagToFind);
BOOL UpdateNodeByTag(const wchar_t* oldTag, const char* newNodeContentJson);
BOOL AddNodeToConfig(const char* newNodeContentJson);
BOOL PinNodeByTag(const wchar_t* tagToPin);
int DeduplicateNodes();
BOOL SortNodesByName();
int FixDuplicateTags(); // 启动时自动修复功能

// --- 重构：新增日志查看器函数声明 ---
void OpenLogViewerWindow();
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// --- 重构结束 ---


// 辅助函数
void ShowTrayTip(const wchar_t* title, const wchar_t* message) {
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle) - 1);
    nid.szInfoTitle[ARRAYSIZE(nid.szInfoTitle) - 1] = L'\0';
    wcsncpy(nid.szInfo, message, ARRAYSIZE(nid.szInfo) - 1);
    nid.szInfo[ARRAYSIZE(nid.szInfo) - 1] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void ShowError(const wchar_t* title, const wchar_t* message) {
    DWORD errorCode = GetLastError();
    wchar_t* sysMsgBuf = NULL;
    wchar_t fullMessage[1024] = {0};
    wcsncpy(fullMessage, message, ARRAYSIZE(fullMessage) - 1);
    fullMessage[ARRAYSIZE(fullMessage) - 1] = L'\0';
    if (errorCode != 0) {
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPWSTR)&sysMsgBuf, 0, NULL);
        if (sysMsgBuf) {
            wcsncat(fullMessage, L"\n\n系统错误信息:\n", ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            wcsncat(fullMessage, sysMsgBuf, ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            LocalFree(sysMsgBuf);
        }
    }
    MessageBoxW(NULL, fullMessage, title, MB_OK | MB_ICONERROR);
}

BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize) {
    FILE* f = NULL;
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) { return FALSE; }
    fseek(f, 0, SEEK_END);
    *fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*fileSize <= 0) { fclose(f); return FALSE; }
    *buffer = (char*)malloc(*fileSize + 1);
    if (!*buffer) { fclose(f); return FALSE; }
    fread(*buffer, 1, *fileSize, f);
    (*buffer)[*fileSize] = '\0';
    fclose(f);
    return TRUE;
}
void CleanupDynamicNodes() {
    if (nodeTags) {
        for (int i = 0; i < nodeCount; i++) { free(nodeTags[i]); }
        free(nodeTags);
        nodeTags = NULL;
    }
    nodeCount = 0;
    nodeCapacity = 0;
}

BOOL IsWindows8OrGreater() {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32 == NULL) {
        return FALSE;
    }
    FARPROC pFunc = GetProcAddress(hKernel32, "SetProcessMitigationPolicy");
    return (pFunc != NULL);
}

char* ConvertLfToCrlf(const char* input) {
    if (!input) return NULL;

    int lf_count = 0;
    for (const char* p = input; *p; p++) {
        if (*p == '\n' && (p == input || *(p-1) != '\r')) {
            lf_count++;
        }
    }

    if (lf_count == 0) {
        char* output = (char*)malloc(strlen(input) + 1);
        if(output) strcpy(output, input);
        return output;
    }

    size_t new_len = strlen(input) + lf_count;
    char* output = (char*)malloc(new_len + 1);
    if (!output) return NULL;

    char* dest = output;
    for (const char* src = input; *src; src++) {
        if (*src == '\n' && (src == input || *(src-1) != '\r')) {
            *dest++ = '\r';
            *dest++ = '\n';
        } else {
            *dest++ = *src;
        }
    }
    *dest = '\0';

    return output;
}

// 快捷键设置功能函数
void LoadSettings() {
    g_hotkeyModifiers = GetPrivateProfileIntW(L"Settings", L"Modifiers", 0, g_iniFilePath);
    g_hotkeyVk = GetPrivateProfileIntW(L"Settings", L"VK", 0, g_iniFilePath);
    g_isIconVisible = GetPrivateProfileIntW(L"Settings", L"ShowIcon", 1, g_iniFilePath);
}

void SaveSettings() {
    wchar_t buffer[16];
    wsprintfW(buffer, L"%u", g_hotkeyModifiers);
    WritePrivateProfileStringW(L"Settings", L"Modifiers", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%u", g_hotkeyVk);
    WritePrivateProfileStringW(L"Settings", L"VK", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%d", g_isIconVisible);
    WritePrivateProfileStringW(L"Settings", L"ShowIcon", buffer, g_iniFilePath);
}

void ToggleTrayIconVisibility() {
    if (g_isIconVisible) { Shell_NotifyIconW(NIM_DELETE, &nid); }
    else { Shell_NotifyIconW(NIM_ADD, &nid); }
    g_isIconVisible = !g_isIconVisible;
    SaveSettings();
}

UINT HotkeyfToMod(UINT flags) {
    UINT mods = 0;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (flags & HOTKEYF_EXT) mods |= MOD_WIN;
    return mods;
}

UINT ModToHotkeyf(UINT mods) {
    UINT flags = 0;
    if (mods & MOD_ALT) flags |= HOTKEYF_ALT;
    if (mods & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (mods & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (mods & MOD_WIN) flags |= HOTKEYF_EXT;
    return flags;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hHotkey, hLabel, hOkBtn, hCancelBtn;
    switch (msg) {
        case WM_CREATE: {
            hLabel = CreateWindowW(L"STATIC", L"请按下快捷键:", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, NULL, NULL);
            hHotkey = CreateWindowExW(0, HOTKEY_CLASSW, NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 45, 240, 25, hWnd, (HMENU)ID_HOTKEY_CTRL, NULL, NULL);
            SendMessageW(hHotkey, HKM_SETHOTKEY, MAKEWORD(g_hotkeyVk, ModToHotkeyf(g_hotkeyModifiers)), 0);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 60, 85, 80, 25, hWnd, (HMENU)IDOK, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 160, 85, 80, 25, hWnd, (HMENU)IDCANCEL, NULL, NULL);

            // 应用字体
            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hHotkey, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK: {
                    LRESULT result = SendMessageW(hHotkey, HKM_GETHOTKEY, 0, 0);
                    UINT newVk = LOBYTE(result);
                    UINT newModsFlags = HIBYTE(result);
                    UINT newMods = HotkeyfToMod(newModsFlags);
                    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
                    if (RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, newMods, newVk)) {
                        g_hotkeyModifiers = newMods; g_hotkeyVk = newVk;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键设置成功！", L"提示", MB_OK);
                    } else if (newVk != 0 || newMods != 0) {
                        MessageBoxW(hWnd, L"快捷键设置失败，可能已被其他程序占用。", L"错误", MB_OK | MB_ICONERROR);
                        if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) { RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk); }
                    } else {
                        g_hotkeyModifiers = 0; g_hotkeyVk = 0;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键已清除。", L"提示", MB_OK);
                    }
                    DestroyWindow(hWnd);
                    break;
                }
                case IDCANCEL: DestroyWindow(hWnd); break;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hWnd); break;
        case WM_DESTROY: EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd); break;
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenSettingsWindow() {
    const wchar_t* SETTINGS_CLASS_NAME = L"SingboxSettingsWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SETTINGS_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!GetClassInfoW(wc.hInstance, SETTINGS_CLASS_NAME, &wc)) { RegisterClassW(&wc); }
    HWND hSettingsWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS_NAME, L"设置快捷键", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300, 160, hwnd, NULL, wc.hInstance, NULL);
    if (hSettingsWnd) {
        EnableWindow(hwnd, FALSE);
        RECT rc, rcOwner;
        GetWindowRect(hSettingsWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hSettingsWnd, HWND_TOP, (rcOwner.right - (rc.right - rc.left)) / 2, (rcOwner.bottom - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE);
        ShowWindow(hSettingsWnd, SW_SHOW);
        UpdateWindow(hSettingsWnd);
    }
}

// =========================================================================
// (已修改) 解析 config.json 以获取节点列表和当前节点 (读取 route.final)
// =========================================================================
BOOL ParseTags() {
    CleanupDynamicNodes();
    currentNode[0] = L'\0';
    httpPort = 0;
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        return FALSE;
    }
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        return FALSE;
    }
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    cJSON* outbound = NULL;
    cJSON_ArrayForEach(outbound, outbounds) {
        cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
        if (cJSON_IsString(tag) && tag->valuestring) {
            if (nodeCount >= nodeCapacity) {
                int newCapacity = (nodeCapacity == 0) ? 10 : nodeCapacity * 2;
                wchar_t** newTags = (wchar_t**)realloc(nodeTags, newCapacity * sizeof(wchar_t*));
                if (!newTags) {
                    cJSON_Delete(root);
                    free(buffer);
                    CleanupDynamicNodes();
                    return FALSE;
                }
                nodeTags = newTags;
                nodeCapacity = newCapacity;
            }
            const char* utf8_str = tag->valuestring;
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
            nodeTags[nodeCount] = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
            if (nodeTags[nodeCount]) {
                MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, nodeTags[nodeCount], wideLen);
                nodeCount++;
            }
        }
    }
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        // (--- 新逻辑 ---)
        // 直接从 route.final 读取当前节点
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (cJSON_IsString(final_outbound) && final_outbound->valuestring) {
            MultiByteToWideChar(CP_UTF8, 0, final_outbound->valuestring, -1, currentNode, ARRAYSIZE(currentNode));
        }
    }
    cJSON* inbounds = cJSON_GetObjectItem(root, "inbounds");
    cJSON* inbound = NULL;
    cJSON_ArrayForEach(inbound, inbounds) {
        cJSON* type = cJSON_GetObjectItem(inbound, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "http") == 0) {
            cJSON* listenPort = cJSON_GetObjectItem(inbound, "listen_port");
            if (cJSON_IsNumber(listenPort)) {
                httpPort = listenPort->valueint;
                break;
            }
        }
    }
    cJSON_Delete(root);
    free(buffer);
    return TRUE;
}


int GetHttpInboundPort() {
    return httpPort;
}


// --- 重构：新增守护线程函数 ---

// 监视 sing-box 核心进程是否崩溃的线程函数
DWORD WINAPI MonitorThread(LPVOID lpParam) {
    HANDLE hProcess = (HANDLE)lpParam;
    
    // 阻塞等待，直到 hProcess 进程终止
    WaitForSingleObject(hProcess, INFINITE);

    // 进程终止后，检查 g_isExiting 标志
    // 如果不是用户主动退出（g_isExiting == FALSE），则向主窗口发送崩溃消息
    if (!g_isExiting) {
        PostMessageW(hwnd, WM_SINGBOX_CRASHED, 0, 0);
    }

    return 0;
}

// 监视 sing-box 核心进程日志输出的线程函数
DWORD WINAPI LogMonitorThread(LPVOID lpParam) {
    char readBuf[4096];      // 原始读取缓冲区
    char lineBuf[8192] = {0}; // 拼接缓冲区，处理跨Read的日志行
    DWORD dwRead;
    BOOL bSuccess;
    static time_t lastLogTriggeredRestart = 0;
    const time_t RESTART_COOLDOWN = 60; // 60秒日志触发冷却
    HANDLE hPipe = (HANDLE)lpParam;

    while (TRUE) {
        // 从管道读取数据
        bSuccess = ReadFile(hPipe, readBuf, sizeof(readBuf) - 1, &dwRead, NULL);
        
        if (!bSuccess || dwRead == 0) {
            // 管道被破坏或关闭 (例如，sing-box 被终止)
            break; // 线程退出
        }

        // 确保缓冲区以NULL结尾
        readBuf[dwRead] = '\0';

        // --- 新增：转发日志到查看器窗口 ---
        if (hLogViewerWnd != NULL && !g_isExiting) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, NULL, 0);
            if (wideLen > 0) {
                // 为 wchar_t* 分配内存
                wchar_t* pWideBuf = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                if (pWideBuf) {
                    MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, pWideBuf, wideLen);
                    
                    // 异步发送消息，将内存指针作为lParam传递
                    // 日志窗口的UI线程将负责 free(pWideBuf)
                    if (!PostMessageW(hLogViewerWnd, WM_LOG_UPDATE, 0, (LPARAM)pWideBuf)) {
                        // 如果PostMessage失败（例如窗口正在关闭），我们必须在这里释放内存
                        free(pWideBuf);
                    }
                }
            }
        }
        // --- 新增结束 ---


        // 将新读取的数据附加到行缓冲区
        strncat(lineBuf, readBuf, sizeof(lineBuf) - strlen(lineBuf) - 1);

        // 如果我们正在退出或切换，不要解析日志
        if (g_isExiting) {
            continue;
        }

        // --- 关键词分析 ---
        // 查找可能需要重启的严重错误
        char* fatal_pos = strstr(lineBuf, "level\"=\"fatal");
        char* dial_pos = strstr(lineBuf, "failed to dial");
        // 新增：检测 timeout 和 connection refused
        char* timeout_pos = strstr(lineBuf, "timeout"); 
        char* refused_pos = strstr(lineBuf, "connection refused");

        if (fatal_pos != NULL || dial_pos != NULL || timeout_pos != NULL || refused_pos != NULL) {
            time_t now = time(NULL);
            if (now - lastLogTriggeredRestart > RESTART_COOLDOWN) {
                lastLogTriggeredRestart = now;
                // 发送重启消息 (现在将触发节点切换)
                PostMessageW(hwnd, WM_SINGBOX_RECONNECT, 0, 0);
            }
            // 处理完错误后，清空缓冲区，防止重复触发
            lineBuf[0] = '\0';
        } else {
            // 如果没有找到错误，我们需要清理缓冲区，只保留最后一行（可能是半行）
            char* last_newline = strrchr(lineBuf, '\n');
            if (last_newline != NULL) {
                // 找到了换行符，只保留换行符之后的内容
                strcpy(lineBuf, last_newline + 1);
            } else if (strlen(lineBuf) > 4096) {
                // 缓冲区已满但没有换行符（异常情况），清空它以防溢出
                lineBuf[0] = '\0';
            }
            // 如果没有换行符且缓冲区未满，则不执行任何操作，等待下一次 ReadFile 拼接
        }
    }
    
    return 0;
}
// --- 重构结束 ---


// --- 重构：修改 StartSingBox ---
void StartSingBox() {
    HANDLE hPipe_Rd_Local = NULL; // 管道读取端（本地）
    HANDLE hPipe_Wr_Local = NULL; // 管道写入端（本地）
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // 创建用于 stdout/stderr 的管道
    if (!CreatePipe(&hPipe_Rd_Local, &hPipe_Wr_Local, &sa, 0)) {
        ShowError(L"管道创建失败", L"无法为核心程序创建输出管道。");
        return;
    }
    // 确保管道的读取句柄不能被子进程继承
    if (!SetHandleInformation(hPipe_Rd_Local, HANDLE_FLAG_INHERIT, 0)) {
        ShowError(L"管道句柄属性设置失败", L"无法设置输出管道读取句柄的属性。");
        CloseHandle(hPipe_Rd_Local);
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    // 将本地读取句柄保存到全局变量，以便日志线程使用
    hChildStd_OUT_Rd_Global = hPipe_Rd_Local;

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hPipe_Wr_Local;
    si.hStdError = hPipe_Wr_Local;

    wchar_t cmdLine[MAX_PATH];
    wcsncpy(cmdLine, L"sing-box.exe run -c config.json", ARRAYSIZE(cmdLine));
    cmdLine[ARRAYSIZE(cmdLine) - 1] = L'\0';

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ShowError(L"核心程序启动失败", L"无法创建 sing-box.exe 进程。");
        ZeroMemory(&pi, sizeof(pi));
        CloseHandle(hChildStd_OUT_Rd_Global); // 清理全局句柄
        hChildStd_OUT_Rd_Global = NULL;
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    // 子进程已继承写入句柄，我们不再需要它
    CloseHandle(hPipe_Wr_Local);

    // 检查核心是否在500ms内立即退出（通常是配置错误）
    if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) {
        char chBuf[4096] = {0};
        DWORD dwRead = 0;
        wchar_t errorOutput[4096] = L"";

        // 从管道读取初始错误输出
        if (ReadFile(hChildStd_OUT_Rd_Global, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead > 0) {
            chBuf[dwRead] = '\0';
            MultiByteToWideChar(CP_UTF8, 0, chBuf, -1, errorOutput, ARRAYSIZE(errorOutput));
        }

        wchar_t fullMessage[8192];
        wsprintfW(fullMessage, L"sing-box.exe 核心程序启动后立即退出。\n\n可能的原因:\n- 配置文件(config.json)格式错误\n- 核心文件损坏或不兼容\n\n核心程序输出:\n%s", errorOutput);
        ShowError(L"核心程序启动失败", fullMessage);
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ZeroMemory(&pi, sizeof(pi));
        
        CloseHandle(hChildStd_OUT_Rd_Global); // 清理管道
        hChildStd_OUT_Rd_Global = NULL;
    } 
    else {
        // --- 进程启动成功，启动监控线程 ---

        // 1. 启动崩溃监控线程
        hMonitorThread = CreateThread(NULL, 0, MonitorThread, pi.hProcess, 0, NULL);
        
        // 2. 启动日志监控线程
        // 我们必须复制管道句柄，因为 LogMonitorThread 会在退出时关闭它
        HANDLE hPipeForLogThread;
        if (DuplicateHandle(GetCurrentProcess(), hChildStd_OUT_Rd_Global,
                           GetCurrentProcess(), &hPipeForLogThread, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        {
            hLogMonitorThread = CreateThread(NULL, 0, LogMonitorThread, hPipeForLogThread, 0, NULL);
        }
        // --- 监控启动完毕 ---
    }

    // 注意：我们 *不* 在这里关闭 hChildStd_OUT_Rd_Global
    // 它由 StopSingBox 统一关闭
}
// --- 重构结束 ---

void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag);
    wcsncpy(currentNode, tag, ARRAYSIZE(currentNode) - 1);
    currentNode[ARRAYSIZE(currentNode)-1] = L'\0';
    
    // --- 重构：添加退出标志 ---
    g_isExiting = TRUE; // 标记为主动操作，防止监控线程误报
    StopSingBox();
    g_isExiting = FALSE; // 清除标志，准备重启
    // --- 重构结束 ---

    StartSingBox();
    wchar_t message[256];
    wsprintfW(message, L"当前节点: %s", tag);
    ShowTrayTip(L"切换成功", message);
}

void SetSystemProxy(BOOL enable) {
    int port = GetHttpInboundPort();
    if (port == 0 && enable) {
        MessageBoxW(NULL, L"未找到HTTP入站端口，无法设置系统代理。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    if (IsWindows8OrGreater()) {
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
            ShowError(L"代理设置失败", L"无法打开注册表键。");
            return;
        }

        if (enable) {
            DWORD dwEnable = 1;
            wchar_t proxyServer[64];
            wsprintfW(proxyServer, L"127.0.0.1:%d", port);
            const wchar_t* proxyBypass = L"<local>";
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)proxyServer, (wcslen(proxyServer) + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, (const BYTE*)proxyBypass, (wcslen(proxyBypass) + 1) * sizeof(wchar_t));
        } else {
            DWORD dwEnable = 0;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
        }
        RegCloseKey(hKey);
    } else {
        INTERNET_PER_CONN_OPTION_LISTW list;
        INTERNET_PER_CONN_OPTIONW options[3];
        DWORD dwBufSize = sizeof(list);
        options[0].dwOption = INTERNET_PER_CONN_FLAGS;
        options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        if (enable) {
            wchar_t proxyServer[64];
            wsprintfW(proxyServer, L"127.0.0.1:%d", port);
            options[0].Value.dwValue = PROXY_TYPE_PROXY;
            options[1].Value.pszValue = proxyServer;
            options[2].Value.pszValue = L"<local>";
        } else {
            options[0].Value.dwValue = PROXY_TYPE_DIRECT;
            options[1].Value.pszValue = L"";
            options[2].Value.pszValue = L"";
        }
        list.dwSize = sizeof(list);
        list.pszConnection = NULL;
        list.dwOptionCount = 3;
        list.dwOptionError = 0;
        list.pOptions = options;
        if (!InternetSetOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, dwBufSize)) {
            ShowError(L"代理设置失败", L"调用 InternetSetOptionW 失败。");
            return;
        }
    }

    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

BOOL IsSystemProxyEnabled() {
    HKEY hKey;
    DWORD dwEnable = 0;
    DWORD dwSize = sizeof(dwEnable);
    wchar_t proxyServer[MAX_PATH] = {0};
    DWORD dwProxySize = sizeof(proxyServer);
    int port = GetHttpInboundPort();
    BOOL isEnabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&dwEnable, &dwSize) == ERROR_SUCCESS) {
            if (dwEnable == 1) {
                if (port > 0) {
                    wchar_t expectedProxyServer[64];
                    wsprintfW(expectedProxyServer, L"127.0.0.1:%d", port);
                    if (RegQueryValueExW(hKey, L"ProxyServer", NULL, NULL, (LPBYTE)proxyServer, &dwProxySize) == ERROR_SUCCESS) {
                        if (wcscmp(proxyServer, expectedProxyServer) == 0) {
                            isEnabled = TRUE;
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return isEnabled;
}

// =========================================================================
// (已修改) 安全地修改 config.json 中的路由 (修改 route.final)
// =========================================================================
void SafeReplaceOutbound(const wchar_t* newTag) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        MessageBoxW(NULL, L"无法打开 config.json", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, newTag, -1, NULL, 0, NULL, NULL);
    char* newTagMb = (char*)malloc(mbLen);
    if (!newTagMb) {
        free(buffer);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, newTag, -1, newTagMb, mbLen, NULL, NULL);
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        free(newTagMb);
        return;
    }

    // (--- 已修改 ---)
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        // (--- 新逻辑 ---)
        // 直接修改 route.final
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (final_outbound) {
            cJSON_SetValuestring(final_outbound, newTagMb);
        } else {
            // (--- 备用逻辑 ---)
            // 如果 "final" 字段不存在，则创建它
            cJSON_AddItemToObject(route, "final", cJSON_CreateString(newTagMb));
        }
    }

    char* newContent = cJSON_PrintBuffered(root, 1, 1);

    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
        }
        free(newContent);
    }
    cJSON_Delete(root);
    free(buffer);
    free(newTagMb);
}

// --- 重构：修改 UpdateMenu ---
void UpdateMenu() {
    if (hMenu) DestroyMenu(hMenu);
    if (hNodeSubMenu) DestroyMenu(hNodeSubMenu);
    hMenu = CreatePopupMenu();
    hNodeSubMenu = CreatePopupMenu();
    for (int i = 0; i < nodeCount; ++i) {
        UINT flags = MF_STRING;
        if (wcscmp(nodeTags[i], currentNode) == 0) { flags |= MF_CHECKED; }
        AppendMenuW(hNodeSubMenu, flags, ID_TRAY_NODE_BASE + i, nodeTags[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNodeSubMenu, L"切换节点");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MANAGE_NODES, L"管理节点");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_CONVERTER, L"节点转换");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"设置快捷键");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"显示日志"); // 新增
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}
// --- 重构结束 ---


// --- 重构：修改 WndProc (增加自动切换节点) ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 自动重启的冷却计时器
    static time_t lastAutoRestart = 0;
    const time_t RESTART_COOLDOWN = 60; // 60秒 (保留定义，用于崩溃提示)

    if (msg == WM_TRAY && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hWnd);
        ParseTags();
        UpdateMenu();
        CheckMenuItem(hMenu, ID_TRAY_AUTORUN, IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hMenu, ID_TRAY_SYSTEM_PROXY, IsSystemProxyEnabled() ? MF_CHECKED : MF_UNCHECKED);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
        PostMessage(hWnd, WM_NULL, 0, 0);
    }
    else if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == ID_TRAY_EXIT) {
            
            g_isExiting = TRUE; // 标记为主动退出

            // --- 新增：销毁日志窗口 ---
            if (hLogViewerWnd != NULL) {
                DestroyWindow(hLogViewerWnd);
            }
            // --- 新增结束 ---

            UnregisterHotKey(hWnd, ID_GLOBAL_HOTKEY);
            if(g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
            if (IsSystemProxyEnabled()) SetSystemProxy(FALSE);
            StopSingBox();
            CleanupDynamicNodes();
            PostQuitMessage(0);
        } else if (id == ID_TRAY_AUTORUN) {
            SetAutorun(!IsAutorunEnabled());
        } else if (id == ID_TRAY_SYSTEM_PROXY) {
            BOOL isEnabled = IsSystemProxyEnabled();
            SetSystemProxy(!isEnabled);
            ShowTrayTip(L"系统代理", isEnabled ? L"系统代理已关闭" : L"系统代理已开启");
        } else if (id == ID_TRAY_OPEN_CONVERTER) {
            OpenConverterHtmlFromResource();
        } else if (id == ID_TRAY_SETTINGS) {
            OpenSettingsWindow();
        } else if (id == ID_TRAY_MANAGE_NODES) {
            OpenNodeManagerWindow();
        } else if (id == ID_TRAY_SHOW_CONSOLE) { // --- 新增：处理日志窗口 ---
            OpenLogViewerWindow();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    } else if (msg == WM_HOTKEY) {
        if (wParam == ID_GLOBAL_HOTKEY) {
            ToggleTrayIconVisibility();
        }
    }
    // --- 重构：处理核心崩溃或日志错误 (新增自动切换) ---
    else if (msg == WM_SINGBOX_CRASHED) {
        // 核心崩溃，只提示，不自动操作
        ShowTrayTip(L"Sing-box 监控", L"核心进程意外终止。请手动检查。");
    }
    else if (msg == WM_SINGBOX_RECONNECT) {
        // 日志检测到错误（如 timeout），执行自动切换节点
        
        // 自动切换的冷却计时器
        static time_t lastAutoSwitch = 0;
        const time_t SWITCH_COOLDOWN = 60; // 60秒冷却，防止频繁切换
        time_t now = time(NULL);

        // 检查是否在冷却时间内
        if (now - lastAutoSwitch > SWITCH_COOLDOWN) {
            lastAutoSwitch = now; // 更新切换时间戳

            // 1. 重新解析以获取最新的节点列表
            ParseTags(); 
            if (nodeCount <= 1) {
                 // 只有一个或没有节点，无需切换
                 ShowTrayTip(L"自动切换", L"检测到连接错误，但无其他节点可切换。");
                 return DefWindowProcW(hWnd, msg, wParam, lParam);
            }

            // 2. 找到当前节点的索引
            int currentIndex = -1;
            for (int i = 0; i < nodeCount; i++) {
                if (wcscmp(nodeTags[i], currentNode) == 0) {
                    currentIndex = i;
                    break;
                }
            }

            // 3. 计算下一个节点的索引（顺序循环切换）
            int nextIndex;
            if (currentIndex == -1 || (currentIndex + 1) >= nodeCount) {
                nextIndex = 0; // 如果找不到当前节点或已是最后一个，则切换到第一个
            } else {
                nextIndex = currentIndex + 1; // 切换到下一个
            }
            
            const wchar_t* nextNodeTag = nodeTags[nextIndex];

            // 4. 避免切换到自己（当只有一个节点时）
            if (wcscmp(currentNode, nextNodeTag) == 0) {
                 ShowTrayTip(L"自动切换", L"检测到连接错误，但无其他节点可切换。");
                 return DefWindowProcW(hWnd, msg, wParam, lParam);
            }

            // 5. 执行切换
            wchar_t message[256];
            wsprintfW(message, L"检测到连接错误，自动切换到: %s", nextNodeTag);
            ShowTrayTip(L"自动切换节点", message);

            // 调用现有的切换函数
            SwitchNode(nextNodeTag);

        }
        // 如果在冷却时间内，则不执行任何操作
    }
    // --- 重构结束 ---
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
// --- 重构结束 ---
// --- 重构：修改 StopSingBox ---
void StopSingBox() {
    // 标记为正在退出，让监控线程自行终止
    g_isExiting = TRUE; 

    // 1. 停止核心进程
    if (pi.hProcess) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (exitCode == STILL_ACTIVE) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    
    // 2. 终止并清理崩溃监控线程
    if (hMonitorThread) {
        // 进程终止后，此线程会很快退出
        WaitForSingleObject(hMonitorThread, 1000);
        CloseHandle(hMonitorThread);
    }

    // 3. 终止并清理日志监控线程
    if (hChildStd_OUT_Rd_Global) {
        // 关闭管道的读取端，这将导致 LogMonitorThread 中的 ReadFile 失败
        CloseHandle(hChildStd_OUT_Rd_Global);
    }
    if (hLogMonitorThread) {
        // 等待日志线程安全退出
        WaitForSingleObject(hLogMonitorThread, 1000);
        CloseHandle(hLogMonitorThread);
    }

    // 4. 重置所有全局句柄
    ZeroMemory(&pi, sizeof(pi));
    hMonitorThread = NULL;
    hLogMonitorThread = NULL;
    hChildStd_OUT_Rd_Global = NULL;
    
    // g_isExiting 会在 StartSingBox 或程序退出前被重置
}
// --- 重构结束 ---

void SetAutorun(BOOL enable) {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

BOOL IsAutorunEnabled() {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE;
}

void OpenConverterHtmlFromResource() {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_HTML_CONVERTER), RT_HTML);
    if (!hRes) {
        MessageBoxW(NULL, L"找不到HTML资源。请在使用ResourceHacker手动嵌入后重试。", L"资源错误", MB_OK|MB_ICONERROR);
        return;
    }
    HGLOBAL hMem = LoadResource(NULL, hRes);
    if (!hMem) return;
    void* pData = LockResource(hMem);
    DWORD dwSize = SizeofResource(NULL, hRes);
    if (!pData || dwSize == 0) return;
    wchar_t tempPath[MAX_PATH], tempFileName[MAX_PATH];
    GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    GetTempFileNameW(tempPath, L"sbx", 0, tempFileName);
    wchar_t* dot = wcsrchr(tempFileName, L'.');
    if (dot) {
        wcsncpy(dot, L".html", (size_t)(tempFileName + ARRAYSIZE(tempFileName) - dot));
    } else {
        wcsncat(tempFileName, L".html", ARRAYSIZE(tempFileName) - wcslen(tempFileName) - 1);
    }
    FILE* f = NULL;
    if (_wfopen_s(&f, tempFileName, L"wb") == 0 && f != NULL) {
        fwrite(pData, 1, dwSize, f);
        fclose(f);
        ShellExecuteW(NULL, L"open", tempFileName, NULL, NULL, SW_SHOWNORMAL);
    }
}

// =========================================================================
// (已修改) 生成默认配置文件 (匹配 route.final 逻辑)
// =========================================================================
void CreateDefaultConfig() {
    // (--- 已修改 ---)
    // 使用一个更完整、更符合当前路由(route.final)结构的默认配置
    const char* defaultConfig =
        "{\n"
        "\t\"log\": {\n"
        "\t\t\"level\": \"debug\",\n" // 匹配用户配置中的日志级别
        "\t\t\"disabled\": false\n"
        "\t},\n"
        "\t\"dns\": {\n"
        "\t\t\"servers\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-direct\",\n"
        "\t\t\t\t\"address\": \"119.29.29.29\",\n" // 腾讯 DNS
        "\t\t\t\t\"detour\": \"直接连接\"\n"
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-proxy\",\n"
        "\t\t\t\t\"address\": \"https://1.1.1.1/dns-query\",\n" // CF DNS
        "\t\t\t\t\"detour\": \"自动切换\"\n" // 假设 '自动切换' 是代理
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-block\",\n"
        "\t\t\t\t\"address\": \"rcode://refused\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"rules\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"domain_suffix\": [\".cn\", \"qq.com\", \"tencent.com\"],\n"
        "\t\t\t\t\"server\": \"dns-direct\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"final\": \"dns-proxy\",\n" // 默认使用代理DNS
        "\t\t\"strategy\": \"ipv4_only\"\n"
        "\t},\n"
        "\t\"inbounds\": [\n"
        "\t\t{\n"
        "\t\t\t\"type\": \"http\",\n"
        "\t\t\t\"tag\": \"http-in\",\n"
        "\t\t\t\"listen\": \"127.0.0.1\",\n"
        "\t\t\t\"listen_port\": 10809\n"
        "\t\t}\n"
        "\t],\n"
        "\t\"outbounds\": [\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"直接连接\",\n"
        "\t\t\t\"type\": \"direct\"\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"阻塞\",\n"
        "\t\t\t\"type\": \"block\"\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"vless-default\",\n" // 默认的占位节点
        "\t\t\t\"type\": \"vless\",\n"
        "\t\t\t\"server\": \"your.server.com\",\n"
        "\t\t\t\"server_port\": 443,\n"
        "\t\t\t\"uuid\": \"00000000-0000-0000-0000-000000000000\",\n"
        "\t\t\t\"tls\": {\n"
        "\t\t\t\t\"enabled\": true,\n"
        "\t\t\t\t\"server_name\": \"your.server.com\"\n"
        "\t\t\t}\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"自动切换\",\n" // 匹配用户配置的 selector
        "\t\t\t\"type\": \"selector\",\n"
        "\t\t\t\"outbounds\": [\n"
        "\t\t\t\t\"vless-default\",\n"
        "\t\t\t\t\"直接连接\",\n"
        "\t\t\t\t\"阻塞\"\n"
        "\t\t\t],\n"
        "\t\t\t\"default\": \"vless-default\"\n"
        "\t\t}\n"
        "\t],\n"
        "\t\"route\": {\n"
        "\t\t\"rules\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"domain_suffix\": [\".cn\", \"qq.com\", \"tencent.com\"],\n"
        "\t\t\t\t\"outbound\": \"直接连接\"\n"
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"ip_cidr\": [\"119.29.29.29\", \"1.1.1.1\"],\n" // DNS服务器
        "\t\t\t\t\"outbound\": \"直接连接\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"final\": \"自动切换\",\n" // (--- 关键修改 ---) 默认路由指向 '自动切换'
        "\t\t\"auto_detect_interface\": true,\n"
        "\t\t\"find_process\": true\n"
        "\t},\n"
        "\t\"experimental\": {\n"
        "\t\t\"clash_api\": {\n"
        "\t\t\t\"external_controller\": \"127.0.0.1:9090\",\n"
        "\t\t\t\"external_ui\": \"ui\",\n"
        "\t\t\t\"default_mode\": \"rule\"\n"
        "\t\t}\n"
        "\t}\n"
        "}";

    FILE* f = NULL;
    if (_wfopen_s(&f, L"config.json", L"wb") == 0 && f != NULL) {
        fwrite(defaultConfig, 1, strlen(defaultConfig), f);
        fclose(f);
        MessageBoxW(NULL,
            L"未找到 config.json，已为您生成默认配置文件。\n\n"
            L"请在使用前修改 config.json 中的 'vless-default' 节点信息。", // (--- 已修改提示 ---)
            L"提示", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(NULL, L"无法创建默认的 config.json 文件。", L"错误", MB_OK | MB_ICONERROR);
    }
}


// =========================================================================
// 节点管理功能实现
// =========================================================================

// 刷新节点管理窗口中的列表框
void RefreshNodeListBox(HWND hListBox) {
    SendMessageW(hListBox, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < nodeCount; i++) {
        SendMessageW(hListBox, LB_ADDSTRING, 0, (LPARAM)nodeTags[i]);
    }
}

// 打开节点管理窗口
void OpenNodeManagerWindow() {
    const wchar_t* MANAGER_CLASS_NAME = L"SingboxNodeManagerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = NodeManagerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = MANAGER_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!GetClassInfoW(wc.hInstance, MANAGER_CLASS_NAME, &wc)) {
        RegisterClassW(&wc);
    }

    HWND hManagerWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, MANAGER_CLASS_NAME, L"管理节点", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 420, 300, hwnd, NULL, wc.hInstance, NULL);
    if (hManagerWnd) {
        EnableWindow(hwnd, FALSE);
        RECT rc, rcOwner;
        GetWindowRect(hManagerWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hManagerWnd, HWND_TOP, (rcOwner.right - (rc.right - rc.left)) / 2, (rcOwner.bottom - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE);
        ShowWindow(hManagerWnd, SW_SHOW);
        UpdateWindow(hManagerWnd);
    }
}

// 节点管理窗口的过程函数
LRESULT CALLBACK NodeManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hListBox, hModifyBtn, hDeleteBtn, hAddBtn, hInfoLabel;

    switch (msg) {
        case WM_CREATE: {
            // 使用 LBS_EXTENDEDSEL 样式以支持多选
            hListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOTIFY, 10, 10, 260, 240, hWnd, (HMENU)ID_NODEMGR_LISTBOX, NULL, NULL);
            hAddBtn = CreateWindowW(L"BUTTON", L"添加节点", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 10, 120, 30, hWnd, (HMENU)ID_NODEMGR_ADD_BTN, NULL, NULL);
            hModifyBtn = CreateWindowW(L"BUTTON", L"修改节点", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 50, 120, 30, hWnd, (HMENU)ID_NODEMGR_MODIFY_BTN, NULL, NULL);
            hDeleteBtn = CreateWindowW(L"BUTTON", L"删除节点", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 90, 120, 30, hWnd, (HMENU)ID_NODEMGR_DELETE_BTN, NULL, NULL);
            hInfoLabel = CreateWindowW(L"STATIC", L"提示：无法删除当前\n正在使用的节点。", WS_CHILD | WS_VISIBLE, 280, 130, 120, 40, hWnd, (HMENU)ID_NODEMGR_INFO_LABEL, NULL, NULL);

            SendMessage(hListBox, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hAddBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hModifyBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hDeleteBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hInfoLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            ParseTags();
            RefreshNodeListBox(hListBox);
            break;
        }
        case WM_CONTEXTMENU: {
            HWND hTargetWnd = (HWND)wParam;
            if (hTargetWnd == hListBox) {
                POINT pt;
                pt.x = LOWORD(lParam);
                pt.y = HIWORD(lParam);

                HMENU hContextMenu = CreatePopupMenu();
                AppendMenuW(hContextMenu, MF_STRING, ID_NODEMGR_CONTEXT_PIN_NODE, L"置顶节点");
                AppendMenuW(hContextMenu, MF_STRING, ID_NODEMGR_CONTEXT_SORT_NODES, L"节点排序");
                AppendMenuW(hContextMenu, MF_STRING, ID_NODEMGR_CONTEXT_DEDUPLICATE, L"节点去重 (内容)");
                // 移除了 "修复重复标签" 菜单项
                AppendMenuW(hContextMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hContextMenu, MF_STRING, ID_NODEMGR_CONTEXT_SELECT_ALL, L"全部选择");
                AppendMenuW(hContextMenu, MF_STRING, ID_NODEMGR_CONTEXT_DESELECT_ALL, L"全部取消");

                TrackPopupMenu(hContextMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hContextMenu);
            }
            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_NODEMGR_CONTEXT_PIN_NODE: {
                    int selCount = SendMessage(hListBox, LB_GETSELCOUNT, 0, 0);
                    if (selCount != 1) {
                        MessageBoxW(hWnd, L"请选择单个节点进行置顶。", L"提示", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    int idx;
                    SendMessage(hListBox, LB_GETSELITEMS, 1, (LPARAM)&idx);
                    if (PinNodeByTag(nodeTags[idx])) {
                        MessageBoxW(hWnd, L"节点已置顶。", L"成功", MB_OK);
                        ParseTags();
                        RefreshNodeListBox(hListBox);
                    } else {
                        MessageBoxW(hWnd, L"置顶失败，请检查配置文件。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    break;
                }
                case ID_NODEMGR_CONTEXT_SORT_NODES: {
                    if (SortNodesByName()) {
                        MessageBoxW(hWnd, L"节点已按名称排序。", L"成功", MB_OK);
                        ParseTags();
                        RefreshNodeListBox(hListBox);
                    } else {
                        MessageBoxW(hWnd, L"节点排序失败，请检查配置文件。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    break;
                }
                case ID_NODEMGR_CONTEXT_DEDUPLICATE: {
                    int removedCount = DeduplicateNodes();
                    if (removedCount >= 0) {
                        wchar_t msg[128];
                        wsprintfW(msg, L"操作完成，成功移除了 %d 个内容重复的节点。", removedCount);
                        MessageBoxW(hWnd, msg, L"去重完成", MB_OK | MB_ICONINFORMATION);
                        ParseTags();
                        RefreshNodeListBox(hListBox);
                    } else {
                        MessageBoxW(hWnd, L"去重操作失败。请检查config.json文件是否可读写或格式是否正确。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    break;
                }
                
                // 移除了 ID_NODEMGR_CONTEXT_FIX_DUPLICATE_TAGS 的 case 处理

                case ID_NODEMGR_CONTEXT_SELECT_ALL:
                    SendMessage(hListBox, LB_SETSEL, TRUE, -1);
                    break;
                case ID_NODEMGR_CONTEXT_DESELECT_ALL:
                    SendMessage(hListBox, LB_SETSEL, FALSE, -1);
                    break;
                case ID_NODEMGR_ADD_BTN: {
                    WNDCLASSW wc = {0};
                    const wchar_t* ADD_CLASS_NAME = L"SingboxAddNodeClass";
                    wc.lpfnWndProc = AddNodeWndProc;
                    wc.hInstance = GetModuleHandleW(NULL);
                    wc.lpszClassName = ADD_CLASS_NAME;
                    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
                    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
                    if (!GetClassInfoW(wc.hInstance, ADD_CLASS_NAME, &wc)) { RegisterClassW(&wc); }

                    EnableWindow(hWnd, FALSE);
                    HWND hAddWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, ADD_CLASS_NAME, L"添加新节点", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 500, 440, hWnd, NULL, wc.hInstance, NULL);

                    MSG msg;
                    while (IsWindow(hAddWnd) && GetMessage(&msg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hAddWnd, &msg)) {
                            TranslateMessage(&msg);
                            DispatchMessage(&msg);
                        }
                    }

                    ParseTags();
                    RefreshNodeListBox(hListBox);
                    EnableWindow(hWnd, TRUE);
                    SetForegroundWindow(hWnd);
                    break;
                }
                case ID_NODEMGR_MODIFY_BTN: {
                    int selCount = SendMessage(hListBox, LB_GETSELCOUNT, 0, 0);
                    if (selCount != 1) {
                        MessageBoxW(hWnd, L"请选择单个节点进行修改。", L"提示", MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                    int idx;
                    SendMessage(hListBox, LB_GETSELITEMS, 1, (LPARAM)&idx);

                    MODIFY_NODE_PARAMS params = {0};
                    wcsncpy(params.oldTag, nodeTags[idx], ARRAYSIZE(params.oldTag) - 1);
                    params.success = FALSE;

                    WNDCLASSW wc = {0};
                    const wchar_t* MODIFY_CLASS_NAME = L"SingboxModifyNodeClass";
                    wc.lpfnWndProc = ModifyNodeWndProc;
                    wc.hInstance = GetModuleHandleW(NULL);
                    wc.lpszClassName = MODIFY_CLASS_NAME;
                    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
                    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
                    if (!GetClassInfoW(wc.hInstance, MODIFY_CLASS_NAME, &wc)) { RegisterClassW(&wc); }

                    EnableWindow(hWnd, FALSE);
                    HWND hModifyWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, MODIFY_CLASS_NAME, L"修改节点内容", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 500, 440, hWnd, NULL, wc.hInstance, &params);

                    MSG msg;
                    while (IsWindow(hModifyWnd) && GetMessage(&msg, NULL, 0, 0)) {
                        if (!IsDialogMessage(hModifyWnd, &msg)) {
                            TranslateMessage(&msg);
                            DispatchMessage(&msg);
                        }
                    }

                    if (params.success) {
                        wchar_t currentTagBeforeParse[256];
                        wcsncpy(currentTagBeforeParse, currentNode, ARRAYSIZE(currentTagBeforeParse) - 1);
                        BOOL wasCurrentNode = (wcscmp(params.oldTag, currentTagBeforeParse) == 0);

                        MessageBoxW(hWnd, L"节点内容修改成功！", L"成功", MB_OK);
                        ParseTags();
                        RefreshNodeListBox(hListBox);

                        if (wasCurrentNode) {
                            MessageBoxW(hWnd, L"检测到当前活动节点已被修改，核心将自动重启以应用更改。", L"提示", MB_OK | MB_ICONINFORMATION);
                            
                            // --- 重构：使用安全重启 ---
                            g_isExiting = TRUE;
                            StopSingBox();
                            g_isExiting = FALSE;
                            StartSingBox();
                            // --- 重构结束 ---
                        }
                    }
                    EnableWindow(hWnd, TRUE);
                    SetForegroundWindow(hWnd);
                    break;
                }
                case ID_NODEMGR_DELETE_BTN: {
                    int selCount = SendMessage(hListBox, LB_GETSELCOUNT, 0, 0);
                    if (selCount == 0) {
                        MessageBoxW(hWnd, L"请至少选择一个要删除的节点。", L"提示", MB_OK | MB_ICONINFORMATION);
                        break;
                    }

                    int* selItems = (int*)malloc(selCount * sizeof(int));
                    if (!selItems) break;
                    SendMessage(hListBox, LB_GETSELITEMS, selCount, (LPARAM)selItems);

                    BOOL deletingCurrent = FALSE;
                    for (int i = 0; i < selCount; i++) {
                        if (wcscmp(nodeTags[selItems[i]], currentNode) == 0) {
                            deletingCurrent = TRUE;
                            break;
                        }
                    }

                    if (deletingCurrent) {
                        MessageBoxW(hWnd, L"无法删除当前正在使用的节点。请取消对当前节点的的选择。", L"操作禁止", MB_OK | MB_ICONWARNING);
                        free(selItems);
                        break;
                    }

                    wchar_t confirmMsg[512];
                    wsprintfW(confirmMsg, L"您确定要删除选中的 %d 个节点吗？\n此操作不可恢复。", selCount);
                    if (MessageBoxW(hWnd, confirmMsg, L"确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        BOOL allSucceeded = TRUE;
                        // 必须从后往前删除，以防索引变化
                        for (int i = selCount - 1; i >= 0; i--) {
                            if (!DeleteNodeByTag(nodeTags[selItems[i]])) {
                                allSucceeded = FALSE;
                            }
                        }

                        if (allSucceeded) {
                            MessageBoxW(hWnd, L"所选节点已成功删除。", L"成功", MB_OK);
                        } else {
                            MessageBoxW(hWnd, L"部分或全部节点删除失败，请检查config.json文件。", L"错误", MB_OK | MB_ICONERROR);
                        }

                        ParseTags();
                        RefreshNodeListBox(hListBox);
                    }
                    free(selItems);
                    break;
                }
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hWnd); break;
        case WM_DESTROY: EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd); break;
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 修改节点内容对话框的过程函数
LRESULT CALLBACK ModifyNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit, hOkBtn, hCancelBtn, hFormatBtn, hLabel;
    static MODIFY_NODE_PARAMS* pParams = NULL;

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
            pParams = (MODIFY_NODE_PARAMS*)pCreate->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pParams);

            hLabel = CreateWindowW(L"STATIC", L"节点内容 (JSON格式):", WS_CHILD | WS_VISIBLE, 15, 10, 200, 20, hWnd, NULL, NULL, NULL);
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 15, 35, 450, 280, hWnd, (HMENU)ID_MODIFY_EDIT_CONTENT, NULL, NULL);
            
            // 调整按钮布局使其对称
            hFormatBtn = CreateWindowW(L"BUTTON", L"JSON格式化", WS_CHILD | WS_VISIBLE, 60, 340, 100, 30, hWnd, (HMENU)ID_MODIFY_FORMAT_BTN, NULL, NULL);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 340, 80, 30, hWnd, (HMENU)ID_MODIFY_OK_BTN, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 360, 340, 80, 30, hWnd, (HMENU)ID_MODIFY_CANCEL_BTN, NULL, NULL);

            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hFormatBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            HFONT hJsonFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
            if(hJsonFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hJsonFont, TRUE);

            char* contentMb = GetNodeContentByTag(pParams->oldTag);
            if (contentMb) {
                char* displayContentMb = ConvertLfToCrlf(contentMb);
                free(contentMb);

                if (displayContentMb) {
                    int wideLen = MultiByteToWideChar(CP_UTF8, 0, displayContentMb, -1, NULL, 0);
                    wchar_t* contentW = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                    if (contentW) {
                        MultiByteToWideChar(CP_UTF8, 0, displayContentMb, -1, contentW, wideLen);
                        SetWindowTextW(hEdit, contentW);
                        free(contentW);
                    }
                    free(displayContentMb);
                }
            } else {
                SetWindowTextW(hEdit, L"// 无法加载节点内容。");
                EnableWindow(hOkBtn, FALSE);
                EnableWindow(hFormatBtn, FALSE);
            }

            RECT rc, rcOwner;
            GetWindowRect(hWnd, &rc);
            GetWindowRect(GetDesktopWindow(), &rcOwner);
            SetWindowPos(hWnd, HWND_TOP,
                rcOwner.left + (rcOwner.right - rcOwner.left - (rc.right - rc.left)) / 2,
                rcOwner.top + (rcOwner.bottom - rcOwner.top - (rc.bottom - rc.top)) / 2,
                0, 0, SWP_NOSIZE);

            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_MODIFY_FORMAT_BTN: {
                    int textLen = GetWindowTextLengthW(hEdit);
                    if (textLen == 0) break;

                    wchar_t* contentW = (wchar_t*)malloc((textLen + 1) * sizeof(wchar_t));
                    if (!contentW) break;
                    GetWindowTextW(hEdit, contentW, textLen + 1);

                    int mbLen = WideCharToMultiByte(CP_UTF8, 0, contentW, -1, NULL, 0, NULL, NULL);
                    char* contentMb = (char*)malloc(mbLen);
                    if (!contentMb) { free(contentW); break; }
                    WideCharToMultiByte(CP_UTF8, 0, contentW, -1, contentMb, mbLen, NULL, NULL);
                    free(contentW);

                    cJSON* json = cJSON_Parse(contentMb);
                    if (!json) {
                        MessageBoxW(hWnd, L"当前内容不是有效的JSON格式，无法格式化。", L"格式化失败", MB_OK | MB_ICONERROR);
                        free(contentMb);
                        break;
                    }

                    char* formattedMb = cJSON_PrintBuffered(json, 1, 1);
                    cJSON_Delete(json);
                    free(contentMb);

                    if (formattedMb) {
                        char* displayFormattedMb = ConvertLfToCrlf(formattedMb);
                        free(formattedMb);

                        if (displayFormattedMb) {
                            int wideLen = MultiByteToWideChar(CP_UTF8, 0, displayFormattedMb, -1, NULL, 0);
                            wchar_t* formattedW = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                            if (formattedW) {
                                MultiByteToWideChar(CP_UTF8, 0, displayFormattedMb, -1, formattedW, wideLen);
                                SetWindowTextW(hEdit, formattedW);
                                free(formattedW);
                            }
                            free(displayFormattedMb);
                        }
                    }
                    break;
                }
                case ID_MODIFY_OK_BTN: {
                    int textLen = GetWindowTextLengthW(hEdit);
                    if (textLen == 0) {
                        MessageBoxW(hWnd, L"节点内容不能为空。", L"错误", MB_OK | MB_ICONERROR);
                        break;
                    }
                    wchar_t* newContentW = (wchar_t*)malloc((textLen + 1) * sizeof(wchar_t));
                    if (!newContentW) break;
                    GetWindowTextW(hEdit, newContentW, textLen + 1);

                    int mbLen = WideCharToMultiByte(CP_UTF8, 0, newContentW, -1, NULL, 0, NULL, NULL);
                    char* newContentMb = (char*)malloc(mbLen);
                    if (!newContentMb) { free(newContentW); break; }
                    WideCharToMultiByte(CP_UTF8, 0, newContentW, -1, newContentMb, mbLen, NULL, NULL);
                    free(newContentW);

                    cJSON* newNodeJson = cJSON_Parse(newContentMb);
                    if (!newNodeJson) {
                        MessageBoxW(hWnd, L"内容不是有效的JSON格式。", L"错误", MB_OK | MB_ICONERROR);
                        free(newContentMb);
                        break;
                    }

                    cJSON* newTagJson = cJSON_GetObjectItem(newNodeJson, "tag");
                    if (!cJSON_IsString(newTagJson) || !newTagJson->valuestring || strlen(newTagJson->valuestring) == 0) {
                        MessageBoxW(hWnd, L"JSON内容中必须包含一个有效的 'tag' 字符串。", L"错误", MB_OK | MB_ICONERROR);
                        cJSON_Delete(newNodeJson);
                        free(newContentMb);
                        break;
                    }

                    int newTagWLen = MultiByteToWideChar(CP_UTF8, 0, newTagJson->valuestring, -1, NULL, 0);
                    wchar_t* newTagW = (wchar_t*)malloc(newTagWLen * sizeof(wchar_t));
                    if (newTagW) {
                         MultiByteToWideChar(CP_UTF8, 0, newTagJson->valuestring, -1, newTagW, newTagWLen);
                         if (wcscmp(pParams->oldTag, newTagW) != 0) {
                             BOOL duplicate = FALSE;
                             for (int i = 0; i < nodeCount; i++) {
                                 if (wcscmp(nodeTags[i], newTagW) == 0) { duplicate = TRUE; break; }
                             }
                             if (duplicate) {
                                 MessageBoxW(hWnd, L"修改后的节点名称已存在，请使用其他名称。", L"错误", MB_OK | MB_ICONERROR);
                                 cJSON_Delete(newNodeJson); free(newContentMb); free(newTagW);
                                 return 0;
                             }
                         }
                         wcsncpy(pParams->newTag, newTagW, ARRAYSIZE(pParams->newTag) - 1);
                         free(newTagW);
                    }
                    cJSON_Delete(newNodeJson);

                    if (UpdateNodeByTag(pParams->oldTag, newContentMb)) {
                        pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                        pParams->success = TRUE;
                        DestroyWindow(hWnd);
                    } else {
                        pParams->success = FALSE;
                        MessageBoxW(hWnd, L"修改失败，请检查配置文件是否可写或格式是否正确。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    free(newContentMb);
                    break;
                }
                case ID_MODIFY_CANCEL_BTN:
                    pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    pParams->success = FALSE;
                    DestroyWindow(hWnd);
                    break;
            }
            break;
        }
        case WM_CLOSE:
            pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            pParams->success = FALSE;
            DestroyWindow(hWnd);
            break;
        case WM_DESTROY: {
             HFONT hFont = (HFONT)SendMessage(GetDlgItem(hWnd, ID_MODIFY_EDIT_CONTENT), WM_GETFONT, 0, 0);
             if (hFont) DeleteObject(hFont);
             break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 添加新节点对话框的过程函数
LRESULT CALLBACK AddNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit, hOkBtn, hCancelBtn, hFormatBtn, hLabel;

    switch (msg) {
        case WM_CREATE: {
            hLabel = CreateWindowW(L"STATIC", L"新节点内容 (JSON格式):", WS_CHILD | WS_VISIBLE, 15, 10, 200, 20, hWnd, NULL, NULL, NULL);
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 15, 35, 450, 280, hWnd, (HMENU)ID_ADD_EDIT_CONTENT, NULL, NULL);

            // 调整按钮布局使其对称
            hFormatBtn = CreateWindowW(L"BUTTON", L"JSON格式化", WS_CHILD | WS_VISIBLE, 60, 340, 100, 30, hWnd, (HMENU)ID_ADD_FORMAT_BTN, NULL, NULL);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 340, 80, 30, hWnd, (HMENU)ID_ADD_OK_BTN, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 360, 340, 80, 30, hWnd, (HMENU)ID_ADD_CANCEL_BTN, NULL, NULL);

            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hFormatBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            HFONT hJsonFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
            if(hJsonFont) SendMessage(hEdit, WM_SETFONT, (WPARAM)hJsonFont, TRUE);

            RECT rc, rcOwner;
            GetWindowRect(hWnd, &rc);
            GetWindowRect(GetDesktopWindow(), &rcOwner);
            SetWindowPos(hWnd, HWND_TOP,
                rcOwner.left + (rcOwner.right - rcOwner.left - (rc.right - rc.left)) / 2,
                rcOwner.top + (rcOwner.bottom - rcOwner.top - (rc.bottom - rc.top)) / 2,
                0, 0, SWP_NOSIZE);

            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_ADD_FORMAT_BTN: {
                    int textLen = GetWindowTextLengthW(hEdit);
                    if (textLen == 0) break;

                    wchar_t* contentW = (wchar_t*)malloc((textLen + 1) * sizeof(wchar_t));
                    if (!contentW) break;
                    GetWindowTextW(hEdit, contentW, textLen + 1);

                    int mbLen = WideCharToMultiByte(CP_UTF8, 0, contentW, -1, NULL, 0, NULL, NULL);
                    char* contentMb = (char*)malloc(mbLen);
                    if (!contentMb) { free(contentW); break; }
                    WideCharToMultiByte(CP_UTF8, 0, contentW, -1, contentMb, mbLen, NULL, NULL);
                    free(contentW);

                    cJSON* json = cJSON_Parse(contentMb);
                    if (!json) {
                        MessageBoxW(hWnd, L"当前内容不是有效的JSON格式，无法格式化。", L"格式化失败", MB_OK | MB_ICONERROR);
                        free(contentMb);
                        break;
                    }

                    char* formattedMb = cJSON_PrintBuffered(json, 1, 1);
                    cJSON_Delete(json);
                    free(contentMb);

                    if (formattedMb) {
                        char* displayFormattedMb = ConvertLfToCrlf(formattedMb);
                        free(formattedMb);

                        if (displayFormattedMb) {
                            int wideLen = MultiByteToWideChar(CP_UTF8, 0, displayFormattedMb, -1, NULL, 0);
                            wchar_t* formattedW = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                            if (formattedW) {
                                MultiByteToWideChar(CP_UTF8, 0, displayFormattedMb, -1, formattedW, wideLen);
                                SetWindowTextW(hEdit, formattedW);
                                free(formattedW);
                            }
                            free(displayFormattedMb);
                        }
                    }
                    break;
                }
                case ID_ADD_OK_BTN: {
                    int textLen = GetWindowTextLengthW(hEdit);
                    if (textLen == 0) {
                        MessageBoxW(hWnd, L"节点内容不能为空。", L"错误", MB_OK | MB_ICONERROR);
                        break;
                    }
                    wchar_t* newContentW = (wchar_t*)malloc((textLen + 1) * sizeof(wchar_t));
                    if (!newContentW) break;
                    GetWindowTextW(hEdit, newContentW, textLen + 1);

                    int mbLen = WideCharToMultiByte(CP_UTF8, 0, newContentW, -1, NULL, 0, NULL, NULL);
                    char* newContentMb = (char*)malloc(mbLen);
                    if (!newContentMb) { free(newContentW); break; }
                    WideCharToMultiByte(CP_UTF8, 0, newContentW, -1, newContentMb, mbLen, NULL, NULL);
                    free(newContentW);

                    cJSON* newNodeJson = cJSON_Parse(newContentMb);
                    if (!newNodeJson) {
                        MessageBoxW(hWnd, L"内容不是有效的JSON格式。", L"错误", MB_OK | MB_ICONERROR);
                        free(newContentMb);
                        break;
                    }

                    cJSON* newTagJson = cJSON_GetObjectItem(newNodeJson, "tag");
                    if (!cJSON_IsString(newTagJson) || !newTagJson->valuestring || strlen(newTagJson->valuestring) == 0) {
                        MessageBoxW(hWnd, L"JSON内容中必须包含一个有效的 'tag' 字符串。", L"错误", MB_OK | MB_ICONERROR);
                        cJSON_Delete(newNodeJson);
                        free(newContentMb);
                        break;
                    }

                    int newTagWLen = MultiByteToWideChar(CP_UTF8, 0, newTagJson->valuestring, -1, NULL, 0);
                    wchar_t* newTagW = (wchar_t*)malloc(newTagWLen * sizeof(wchar_t));
                    if (newTagW) {
                         MultiByteToWideChar(CP_UTF8, 0, newTagJson->valuestring, -1, newTagW, newTagWLen);
                         BOOL duplicate = FALSE;
                         for (int i = 0; i < nodeCount; i++) {
                             if (wcscmp(nodeTags[i], newTagW) == 0) { duplicate = TRUE; break; }
                         }
                         free(newTagW);
                         if (duplicate) {
                             // 启动时已自动修复，但此处保留检查以防万一
                             MessageBoxW(hWnd, L"节点名称已存在，请使用其他名称。\n(程序启动时会自动修复重复标签)", L"错误", MB_OK | MB_ICONERROR);
                             cJSON_Delete(newNodeJson); free(newContentMb);
                             return 0;
                         }
                    }
                    cJSON_Delete(newNodeJson);

                    if (AddNodeToConfig(newContentMb)) {
                        MessageBoxW(GetParent(hWnd), L"节点添加成功！", L"成功", MB_OK);
                        DestroyWindow(hWnd);
                    } else {
                        MessageBoxW(hWnd, L"添加失败，请检查配置文件是否可写或格式是否正确。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    free(newContentMb);
                    break;
                }
                case ID_ADD_CANCEL_BTN:
                    DestroyWindow(hWnd);
                    break;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hWnd);
            break;
        case WM_DESTROY: {
             HFONT hFont = (HFONT)SendMessage(GetDlgItem(hWnd, ID_ADD_EDIT_CONTENT), WM_GETFONT, 0, 0);
             if (hFont) DeleteObject(hFont);
             break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}


// =========================================================================
// (已修改) 从配置文件中删除指定tag的节点
// (新增) 同步更新 "自动切换" selector 列表
// =========================================================================
BOOL DeleteNodeByTag(const wchar_t* tagToDelete) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return FALSE;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return FALSE;

    int mbLen = WideCharToMultiByte(CP_UTF8, 0, tagToDelete, -1, NULL, 0, NULL, NULL);
    char* tagToDeleteMb = (char*)malloc(mbLen);
    if (!tagToDeleteMb) { cJSON_Delete(root); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, tagToDelete, -1, tagToDeleteMb, mbLen, NULL, NULL);

    BOOL success = FALSE;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (!cJSON_IsArray(outbounds)) {
        cJSON_Delete(root);
        free(tagToDeleteMb);
        return FALSE;
    }
    
    // (--- 新增逻辑：从 "自动切换" selector 中移除 tag ---)
    // 1. 检查是否为特殊节点 (这些节点不应在 selector 中)
    if (strcmp(tagToDeleteMb, "直接连接") != 0 &&
        strcmp(tagToDeleteMb, "阻塞") != 0 &&
        strcmp(tagToDeleteMb, "自动切换") != 0)
    {
        // 2. 遍历 outbounds 找到 "自动切换" 节点
        cJSON* outbound_iter = NULL;
        cJSON_ArrayForEach(outbound_iter, outbounds) {
            cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
            cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
            
            if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "自动切换") == 0 &&
                cJSON_IsString(type_item) && strcmp(type_item->valuestring, "selector") == 0)
            {
                // 3. 找到了 selector, 遍历其内部的 outbounds 数组
                cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                if (cJSON_IsArray(selector_outbounds)) {
                    int i = 0;
                    cJSON* selector_tag_item = NULL;
                    cJSON_ArrayForEach(selector_tag_item, selector_outbounds) {
                        // 4. 找到匹配的 tag 字符串并删除
                        if (cJSON_IsString(selector_tag_item) && strcmp(selector_tag_item->valuestring, tagToDeleteMb) == 0) {
                            cJSON_DeleteItemFromArray(selector_outbounds, i);
                            break; // 假设 tag 在 selector 中只出现一次
                        }
                        i++;
                    }
                }
                break; // 假设只有一个 "自动切换" selector
            }
        }
    }
    // (--- 新增逻辑结束 ---)


    // (--- 原有逻辑：从主 outbounds 数组中移除节点对象 ---)
    int i = 0;
    cJSON* outbound = NULL;
    cJSON_ArrayForEach(outbound, outbounds) {
        cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
        if (cJSON_IsString(tag) && strcmp(tag->valuestring, tagToDeleteMb) == 0) {
            cJSON_DeleteItemFromArray(outbounds, i);
            success = TRUE;
            break;
        }
        i++;
    }

    if (success) {
        char* newContent = cJSON_PrintBuffered(root, 1, 1);

        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            }
            free(newContent);
        } else {
            success = FALSE;
        }
    }

    cJSON_Delete(root);
    free(tagToDeleteMb);
    return success;
}


// 通过Tag获取节点的完整JSON内容（返回的字符串需要手动free）
char* GetNodeContentByTag(const wchar_t* tagToFind) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return NULL;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return NULL;

    int mbLen = WideCharToMultiByte(CP_UTF8, 0, tagToFind, -1, NULL, 0, NULL, NULL);
    char* tagToFindMb = (char*)malloc(mbLen);
    if (!tagToFindMb) { cJSON_Delete(root); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, tagToFind, -1, tagToFindMb, mbLen, NULL, NULL);

    char* content = NULL;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (cJSON_IsArray(outbounds)) {
        cJSON* outbound = NULL;
        cJSON_ArrayForEach(outbound, outbounds) {
            cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
            if (cJSON_IsString(tag) && strcmp(tag->valuestring, tagToFindMb) == 0) {
                content = cJSON_PrintBuffered(outbound, 1, 1);
                break;
            }
        }
    }

    cJSON_Delete(root);
    free(tagToFindMb);
    return content;
}


// =========================================================================
// (已修改) 通过Tag更新节点的完整JSON内容
// (新增) 同步更新 "自动切换" selector 列表中的 tag 名称
// =========================================================================
BOOL UpdateNodeByTag(const wchar_t* oldTag, const char* newNodeContentJson) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return FALSE;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return FALSE;

    cJSON* newNode = cJSON_Parse(newNodeContentJson);
    if (!newNode) { cJSON_Delete(root); return FALSE; }

    int oldTagMbLen = WideCharToMultiByte(CP_UTF8, 0, oldTag, -1, NULL, 0, NULL, NULL);
    char* oldTagMb = (char*)malloc(oldTagMbLen);
    if (!oldTagMb) { cJSON_Delete(root); cJSON_Delete(newNode); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, oldTag, -1, oldTagMb, oldTagMbLen, NULL, NULL);

    BOOL success = FALSE;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (cJSON_IsArray(outbounds)) {
        int i = 0;
        cJSON* outbound = NULL;
        cJSON_ArrayForEach(outbound, outbounds) {
            cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
            if (cJSON_IsString(tag) && strcmp(tag->valuestring, oldTagMb) == 0) {
                if (cJSON_ReplaceItemInArray(outbounds, i, newNode)) {
                    success = TRUE;
                }
                break;
            }
            i++;
        }
    }

    if (!success) {
        cJSON_Delete(newNode); // 替换失败，必须删除新节点
    }

    if (success && wcscmp(oldTag, currentNode) == 0) {
        cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
        const char* newTagMb = newTagJson->valuestring;

        if (strcmp(oldTagMb, newTagMb) != 0) {
            // (--- 原有逻辑：更新 route.final ---)
            cJSON* route = cJSON_GetObjectItem(root, "route");
            if (route) {
                cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
                if (final_outbound && strcmp(final_outbound->valuestring, oldTagMb) == 0) {
                    cJSON_SetValuestring(final_outbound, newTagMb);
                }
            }
            MultiByteToWideChar(CP_UTF8, 0, newTagMb, -1, currentNode, ARRAYSIZE(currentNode));
        }
    }
    
    // (--- 新增逻辑：如果 tag 名称改变，同步更新 "自动切换" selector ---)
    if (success) {
        cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
        const char* newTagMb = newTagJson->valuestring;

        // 仅在 tag 确实发生变化时执行
        if (strcmp(oldTagMb, newTagMb) != 0) {
            // 1. 检查是否为特殊节点
            if (strcmp(oldTagMb, "直接连接") != 0 &&
                strcmp(oldTagMb, "阻塞") != 0 &&
                strcmp(oldTagMb, "自动切换") != 0)
            {
                // 2. 遍历 outbounds 找到 "自动切换" 节点
                cJSON* outbound_iter = NULL;
                cJSON_ArrayForEach(outbound_iter, outbounds) {
                    cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
                    cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
                    
                    if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "自动切换") == 0 &&
                        cJSON_IsString(type_item) && strcmp(type_item->valuestring, "selector") == 0)
                    {
                        // 3. 找到了 selector, 遍历其内部的 outbounds 数组
                        cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                        if (cJSON_IsArray(selector_outbounds)) {
                            cJSON* selector_tag_item = NULL;
                            cJSON_ArrayForEach(selector_tag_item, selector_outbounds) {
                                // 4. 找到匹配的旧 tag 字符串并替换为新 tag
                                if (cJSON_IsString(selector_tag_item) && strcmp(selector_tag_item->valuestring, oldTagMb) == 0) {
                                    cJSON_SetValuestring(selector_tag_item, newTagMb);
                                    break; 
                                }
                            }
                        }
                        break; 
                    }
                }
            }
        }
    }
    // (--- 新增逻辑结束 ---)


    if (success) {
        char* newContent = cJSON_PrintBuffered(root, 1, 1);

        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            }
            free(newContent);
        } else {
            success = FALSE;
        }
    }

    cJSON_Delete(root);
    free(oldTagMb);
    return success;
}

// =========================================================================
// (已修改) 向配置文件中添加新节点
// (新增) 同步更新 "自动切换" selector 列表
// =========================================================================
BOOL AddNodeToConfig(const char* newNodeContentJson) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return FALSE;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return FALSE;

    cJSON* newNode = cJSON_Parse(newNodeContentJson);
    if (!newNode) {
        cJSON_Delete(root);
        return FALSE;
    }

    // (--- 新增逻辑：获取新节点的 tag ---)
    cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
    if (!cJSON_IsString(newTagJson) || !newTagJson->valuestring || strlen(newTagJson->valuestring) == 0) {
        // 节点必须有 tag (在 AddNodeWndProc 中已检查, 此处为安全冗余)
        cJSON_Delete(newNode);
        cJSON_Delete(root);
        return FALSE;
    }
    const char* newTagMb = newTagJson->valuestring;
    // (--- 新增逻辑结束 ---)


    BOOL success = FALSE;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (cJSON_IsArray(outbounds)) {
        // (--- 原有逻辑 ---)
        cJSON_AddItemToArray(outbounds, newNode);
        success = TRUE;

        // (--- 新增逻辑：将新 tag 添加到 "自动切换" selector ---)
        // 1. 检查是否为特殊节点
        if (strcmp(newTagMb, "直接连接") != 0 &&
            strcmp(newTagMb, "阻塞") != 0 &&
            strcmp(newTagMb, "自动切换") != 0)
        {
            // 2. 遍历 outbounds 找到 "自动切换" 节点
            cJSON* outbound_iter = NULL;
            cJSON_ArrayForEach(outbound_iter, outbounds) {
                cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
                cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
                
                if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "自动切换") == 0 &&
                    cJSON_IsString(type_item) && strcmp(type_item->valuestring, "selector") == 0)
                {
                    // 3. 找到了 selector, 获取其内部的 outbounds 数组
                    cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                    if (cJSON_IsArray(selector_outbounds)) {
                        // 4. 将新节点的 tag 字符串添加进去
                        cJSON_AddItemToArray(selector_outbounds, cJSON_CreateString(newTagMb));
                    }
                    break; 
                }
            }
        }
        // (--- 新增逻辑结束 ---)

    } else {
        // 如果连主 outbounds 都没有，直接释放新节点
        cJSON_Delete(newNode);
    }

    if (success) {
        char* newContent = cJSON_PrintBuffered(root, 1, 1);

        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            } else {
                success = FALSE;
            }
            free(newContent);
        } else {
            success = FALSE;
        }
    }

    cJSON_Delete(root);
    return success;
}

// 将指定tag的节点移动到outbounds数组的第一个位置
BOOL PinNodeByTag(const wchar_t* tagToPin) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return FALSE;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return FALSE;

    int mbLen = WideCharToMultiByte(CP_UTF8, 0, tagToPin, -1, NULL, 0, NULL, NULL);
    char* tagToPinMb = (char*)malloc(mbLen);
    if (!tagToPinMb) { cJSON_Delete(root); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, tagToPin, -1, tagToPinMb, mbLen, NULL, NULL);

    BOOL success = FALSE;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (cJSON_IsArray(outbounds)) {
        int i = 0;
        cJSON* outbound = NULL;
        cJSON* nodeToPin = NULL;
        cJSON_ArrayForEach(outbound, outbounds) {
            cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
            if (cJSON_IsString(tag) && strcmp(tag->valuestring, tagToPinMb) == 0) {
                nodeToPin = cJSON_DetachItemFromArray(outbounds, i);
                break;
            }
            i++;
        }
        if (nodeToPin) {
            cJSON_AddItemToArray(outbounds, nodeToPin); // First add to end to handle memory
            cJSON* last = cJSON_DetachItemFromArray(outbounds, cJSON_GetArraySize(outbounds)-1); // Detach it again from the end
            cJSON_InsertItemInArray(outbounds, 0, last); // Insert at the beginning
            success = TRUE;
        }
    }

    if (success) {
        char* newContent = cJSON_PrintBuffered(root, 1, 1);
        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            } else {
                success = FALSE;
            }
            free(newContent);
        } else {
            success = FALSE;
        }
    }

    cJSON_Delete(root);
    free(tagToPinMb);
    return success;
}

// 移除配置文件中的重复节点, 返回移除的数量, -1表示失败
int DeduplicateNodes() {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return -1;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return -1;

    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (!cJSON_IsArray(outbounds)) {
        cJSON_Delete(root);
        return -1;
    }

    int original_count = cJSON_GetArraySize(outbounds);
    if (original_count <= 1) {
        cJSON_Delete(root);
        return 0;
    }

    // 将当前节点 wchar_t* 转换为 char*
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, NULL, 0, NULL, NULL);
    char* currentNodeMb = (char*)malloc(mbLen);
    if (!currentNodeMb) {
        cJSON_Delete(root);
        return -1;
    }
    WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, currentNodeMb, mbLen, NULL, NULL);

    cJSON* new_outbounds = cJSON_CreateArray();
    char** seen_fingerprints = (char**)calloc(original_count, sizeof(char*));
    int seen_count = 0;
    int removed_count = 0;

    cJSON* node = NULL;
    cJSON_ArrayForEach(node, outbounds) {
        cJSON* temp_node = cJSON_Duplicate(node, 1);
        cJSON_DeleteItemFromObject(temp_node, "tag");
        char* fingerprint = cJSON_PrintUnformatted(temp_node);
        cJSON_Delete(temp_node);

        BOOL is_duplicate = FALSE;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen_fingerprints[i], fingerprint) == 0) {
                is_duplicate = TRUE;
                break;
            }
        }

        BOOL is_current_node = FALSE;
        cJSON* tag_item = cJSON_GetObjectItem(node, "tag");
        if (cJSON_IsString(tag_item) && tag_item->valuestring) {
            if (strcmp(tag_item->valuestring, currentNodeMb) == 0) {
                is_current_node = TRUE;
            }
        }
        
        // 如果是重复节点，并且不是当前正在使用的节点，则跳过（即删除）
        if (is_duplicate && !is_current_node) {
            removed_count++;
            free(fingerprint);
            continue;
        }

        // 否则，保留该节点
        cJSON_AddItemToArray(new_outbounds, cJSON_Duplicate(node, 1));

        // 如果该指纹是第一次见到，则记录下来
        if (!is_duplicate) {
            seen_fingerprints[seen_count++] = fingerprint;
        } else {
            free(fingerprint); // 如果是重复的指纹，则释放内存
        }
    }

    // 释放所有指纹字符串
    for (int i = 0; i < seen_count; i++) {
        free(seen_fingerprints[i]);
    }
    free(seen_fingerprints);
    free(currentNodeMb);

    // 用新的去重后的数组替换旧数组
    cJSON_ReplaceItemInObject(root, "outbounds", new_outbounds);

    // 写回文件
    BOOL success = FALSE;
    char* newContent = cJSON_PrintBuffered(root, 1, 1);
    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
            success = TRUE;
        }
        free(newContent);
    }
    
    cJSON_Delete(root);

    return success ? removed_count : -1;
}

// 用于qsort的比较函数，根据节点 "tag" 字段进行排序
static int compare_nodes_by_name(const void* a, const void* b) {
    const cJSON* nodeA = *(const cJSON**)a;
    const cJSON* nodeB = *(const cJSON**)b;

    cJSON* tagA_item = cJSON_GetObjectItem(nodeA, "tag");
    cJSON* tagB_item = cJSON_GetObjectItem(nodeB, "tag");

    // 安全地获取tag字符串，如果tag不存在或不是字符串，则视为空字符串
    const char* tagA = (cJSON_IsString(tagA_item) && tagA_item->valuestring) ? tagA_item->valuestring : "";
    const char* tagB = (cJSON_IsString(tagB_item) && tagB_item->valuestring) ? tagB_item->valuestring : "";

    return strcmp(tagA, tagB);
}

// 按名称对配置文件中的节点进行排序
BOOL SortNodesByName() {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return FALSE;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return FALSE;

    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (!cJSON_IsArray(outbounds)) {
        cJSON_Delete(root);
        return FALSE;
    }

    int count = cJSON_GetArraySize(outbounds);
    if (count <= 1) {
        cJSON_Delete(root);
        return TRUE; // 只有一个或没有节点，无需排序
    }

    // 创建一个 cJSON 指针数组来保存分离出的节点
    cJSON** nodes = (cJSON**)malloc(count * sizeof(cJSON*));
    if (!nodes) {
        cJSON_Delete(root);
        return FALSE;
    }

    // 从 outbounds 数组中分离出所有节点
    for (int i = 0; i < count; i++) {
        nodes[i] = cJSON_DetachItemFromArray(outbounds, 0); // 每次都分离第一个
    }

    // 使用 qsort 对节点指针数组进行排序
    qsort(nodes, count, sizeof(cJSON*), compare_nodes_by_name);

    // 将排序后的节点重新添加回空的 outbounds 数组中
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(outbounds, nodes[i]);
    }

    // 释放指针数组本身（节点已被重新附加，不应释放）
    free(nodes);

    BOOL success = FALSE;
    char* newContent = cJSON_PrintBuffered(root, 1, 1);
    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
            success = TRUE;
        }
        free(newContent);
    }

    cJSON_Delete(root);
    return success;
}

/**
 * @brief 自动修复配置文件中重复的 "tag" 名称。
 * * 遍历所有 outbounds，如果发现 "tag" 名称已存在，
 * 则自动附加 "(1)", "(2)" 等后缀，直到名称唯一。
 * * 同时，如果当前活动节点被重命名，会自动更新 'route' 部分。
 * * @return int 成功重命名的节点数量。
 * 0   表示没有发现重复项。
 * -1  表示发生错误 (如文件读写失败, JSON格式错误)。
 */
int FixDuplicateTags() {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) return -1;

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) return -1;

    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (!cJSON_IsArray(outbounds)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(outbounds);
    if (count <= 1) {
        cJSON_Delete(root);
        return 0; // 只有一个或没有节点，不可能重复
    }

    // 创建一个 "已使用标签" 列表
    // 使用 calloc 确保所有指针初始化为 NULL
    char** seenTags = (char**)calloc(count, sizeof(char*));
    if (!seenTags) {
        cJSON_Delete(root);
        return -1;
    }
    int seenCount = 0;
    int renamedCount = 0;
    BOOL hasChanges = FALSE;
    char* currentActiveTagMb = NULL;
    
    // 转换当前活动节点标签 (wchar_t -> char*)
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, NULL, 0, NULL, NULL);
    currentActiveTagMb = (char*)malloc(mbLen);
    if (currentActiveTagMb) {
         WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, currentActiveTagMb, mbLen, NULL, NULL);
    }


    cJSON* node = NULL;
    cJSON_ArrayForEach(node, outbounds) {
        cJSON* tagItem = cJSON_GetObjectItem(node, "tag");
        if (!cJSON_IsString(tagItem) || !tagItem->valuestring || strlen(tagItem->valuestring) == 0) {
            continue; // 跳过没有标签或标签为空的节点
        }

        char* currentTag = tagItem->valuestring;
        BOOL isDuplicate = FALSE;

        // 检查 "seenTags" 列表中是否已存在该标签
        for (int i = 0; i < seenCount; i++) {
            if (strcmp(seenTags[i], currentTag) == 0) {
                isDuplicate = TRUE;
                break;
            }
        }

        if (isDuplicate) {
            // 发现重复标签，必须重命名
            int suffix = 1;
            char newTagBuffer[512]; // 假设标签名不会超长
            BOOL uniqueTagFound = FALSE;

            while (!uniqueTagFound) {
                // 构造新标签名，例如 "SEA-vless (1)"
                snprintf(newTagBuffer, sizeof(newTagBuffer), "%s (%d)", currentTag, suffix);

                // 检查这个 *新构造* 的标签是否也已存在
                BOOL newTagConflict = FALSE;
                for (int i = 0; i < seenCount; i++) {
                    if (strcmp(seenTags[i], newTagBuffer) == 0) {
                        newTagConflict = TRUE;
                        break;
                    }
                }

                if (!newTagConflict) {
                    // 找到了一个独一无二的新名称
                    uniqueTagFound = TRUE;
                    
                    BOOL isCurrentActiveNode = (currentActiveTagMb && strcmp(currentTag, currentActiveTagMb) == 0);

                    // 1. 修改 cJSON 'outbounds' 对象中的 "tag" 值
                    cJSON_SetValuestring(tagItem, newTagBuffer);
                    
                    // 2. 如果被重命名的是当前活动节点，
                    //    我们必须 *在同一个 cJSON root* 中更新 'route' 部分
                    if (isCurrentActiveNode) {
                        cJSON* route = cJSON_GetObjectItem(root, "route");
                        if (route) {
                            // (--- 已修改 ---)
                            // 确保更新 route.final
                            cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
                            // 检查 'final' 是否指向 *旧的* 标签名
                            if (final_outbound && strcmp(final_outbound->valuestring, currentActiveTagMb) == 0) {
                                cJSON_SetValuestring(final_outbound, newTagBuffer); // 设置为 *新的* 标签名
                            }

                            // (--- 保留旧逻辑以防万一 ---)
                            // (但也可能需要移除，取决于用户配置的复杂性)
                            BOOL updated = FALSE;
                            cJSON* rules = cJSON_GetObjectItem(route, "rules");
                            if (cJSON_IsArray(rules) && cJSON_GetArraySize(rules) > 0) {
                                cJSON* first_rule = cJSON_GetArrayItem(rules, 0);
                                if (first_rule) {
                                    cJSON* rule_outbound = cJSON_GetObjectItem(first_rule, "outbound");
                                    // 检查路由是否指向 *旧的* 标签名
                                    if (rule_outbound && strcmp(rule_outbound->valuestring, currentActiveTagMb) == 0) {
                                        cJSON_SetValuestring(rule_outbound, newTagBuffer); // 设置为 *新的* 标签名
                                        updated = TRUE;
                                    }
                                }
                            }
                        }
                    }

                    // 3. 将这个新名称添加到 "seenTags" 列表中
                    seenTags[seenCount] = strdup(newTagBuffer);
                    if (seenTags[seenCount]) {
                        seenCount++;
                    }
                    
                    renamedCount++;
                    hasChanges = TRUE;
                } else {
                    // 新名称依然冲突 (例如 "Tag (1)" 已存在)，增加后缀重试
                    suffix++;
                }
            }
        } else {
            // 标签不重复，将其添加到 "seenTags" 列表
            seenTags[seenCount] = strdup(currentTag);
            if (seenTags[seenCount]) {
                seenCount++;
            }
        }
    }

    // 释放 "seenTags" 列表及其中的字符串
    for (int i = 0; i < seenCount; i++) {
        free(seenTags[i]);
    }
    free(seenTags);
    if(currentActiveTagMb) free(currentActiveTagMb);

    if (hasChanges) {
        // 如果有修改，则写回 config.json 文件
        char* newContent = cJSON_PrintBuffered(root, 1, 1);
        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            } else {
                renamedCount = -1; // 标记文件写入失败
            }
            free(newContent);
        } else {
            renamedCount = -1; // 标记 cJSON 打印失败
        }
    }

    cJSON_Delete(root);
    return renamedCount;
}


// =========================================================================
// (--- 新增 ---) 日志查看器功能实现
// =========================================================================

// 日志窗口过程函数
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    // 定义日志缓冲区的大小限制，防止窗口因日志过多而卡死
    const int MAX_LOG_LENGTH = 200000;  // 最大字符数
    const int TRIM_LOG_LENGTH = 100000; // 裁剪后保留的字符数

    switch (msg) {
        case WM_CREATE: {
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                    0, 0, 0, 0, // 将在 WM_SIZE 中调整大小
                                    hWnd, (HMENU)ID_LOGVIEWER_EDIT,
                                    GetModuleHandle(NULL), NULL);
            
            if (hEdit == NULL) {
                ShowError(L"创建失败", L"无法创建日志显示框。");
                return -1; // 阻止窗口创建
            }
            
            // 设置等宽字体
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hLogFont, TRUE);
            break;
        }

        case WM_LOG_UPDATE: {
            // 这是从 LogMonitorThread 线程接收到的消息
            wchar_t* pLogChunk = (wchar_t*)lParam;
            if (pLogChunk) {
                // 性能优化：检查是否需要裁剪日志
                int textLen = GetWindowTextLengthW(hEdit);
                if (textLen > MAX_LOG_LENGTH) {
                    // 裁剪：删除前 TRIM_LOG_LENGTH 个字符
                    SendMessageW(hEdit, EM_SETSEL, 0, TRIM_LOG_LENGTH);
                    SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)L"[... 日志已裁剪 ...]\r\n");
                }

                // 追加新文本
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); // 移动到文本末尾
                SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)pLogChunk); // 追加新日志
                
                // 释放由 LogMonitorThread 分配的内存
                free(pLogChunk);
            }
            break;
        }

        case WM_SIZE: {
            // 窗口大小改变时，自动填满 EDIT 控件
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            MoveWindow(hEdit, 0, 0, rcClient.right, rcClient.bottom, TRUE);
            break;
        }

        case WM_CLOSE: {
            // 用户点击关闭时，只隐藏窗口，不销毁
            // 这样下次打开时，日志历史还在
            ShowWindow(hWnd, SW_HIDE);
            hLogViewerWnd = NULL; // 标记为“已关闭”
            break;
        }

        case WM_DESTROY: {
            // 窗口被真正销毁时（例如程序退出时）
            hLogViewerWnd = NULL;
            break;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 打开或显示日志窗口
void OpenLogViewerWindow() {
    if (hLogViewerWnd != NULL) {
        // 窗口已存在，只需显示并置顶
        ShowWindow(hLogViewerWnd, SW_SHOW);
        SetForegroundWindow(hLogViewerWnd);
        return;
    }

    // 窗口不存在，需要创建
    const wchar_t* LOGVIEWER_CLASS_NAME = L"SingboxLogViewerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LogViewerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = LOGVIEWER_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(1)); // 使用主程序图标
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); // 备用图标
    }

    if (!GetClassInfoW(wc.hInstance, LOGVIEWER_CLASS_NAME, &wc)) {
        if (!RegisterClassW(&wc)) {
            ShowError(L"窗口注册失败", L"无法注册日志窗口类。");
            return;
        }
    }

    hLogViewerWnd = CreateWindowExW(
        0, LOGVIEWER_CLASS_NAME, L"Sing-box 实时日志",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 450,
        hwnd, // 父窗口设为主窗口，以便管理
        NULL, wc.hInstance, NULL
    );

    if (hLogViewerWnd) {
        // 尝试将窗口居中
        RECT rc, rcOwner;
        GetWindowRect(hLogViewerWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hLogViewerWnd, HWND_TOP,
            (rcOwner.right - (rc.right - rc.left)) / 2,
            (rcOwner.bottom - (rc.bottom - rc.top)) / 2,
            0, 0, SWP_NOSIZE);
    } else {
        ShowError(L"窗口创建失败", L"无法创建日志窗口。");
    }
}


// =========================================================================
// 主函数
// =========================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    wchar_t mutexName[128];
    wchar_t guidString[40];

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"宋体");

    // --- 新增：创建日志等宽字体 ---
    hLogFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hLogFont == NULL) {
        hLogFont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT); // 备用字体
    }
    // --- 新增结束 ---

    wsprintfW(guidString, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        APP_GUID.Data1, APP_GUID.Data2, APP_GUID.Data3,
        (UINT)APP_GUID.Data4[0], (UINT)APP_GUID.Data4[1], (UINT)APP_GUID.Data4[2], (UINT)APP_GUID.Data4[3],
        (UINT)APP_GUID.Data4[4], (UINT)APP_GUID.Data4[5], (UINT)APP_GUID.Data4[6], (UINT)APP_GUID.Data4[7]);

    wsprintfW(mutexName, L"Global\\%s", guidString);

    hMutex = CreateMutexW(NULL, TRUE, mutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"程序已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        return 0;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&icex);
    wchar_t szPath[MAX_PATH];
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    wchar_t* p = wcsrchr(szPath, L'\\');
    if (p) {
        *p = L'\0';
        SetCurrentDirectoryW(szPath);
        wcsncpy(g_iniFilePath, szPath, MAX_PATH - 1);
        g_iniFilePath[MAX_PATH - 1] = L'\0';
        wcsncat(g_iniFilePath, L"\\set.ini", MAX_PATH - wcslen(g_iniFilePath) - 1);
    } else {
        wcsncpy(g_iniFilePath, L"set.ini", MAX_PATH - 1);
    }

    // 检查config.json是否存在，如果不存在则创建
    DWORD fileAttr = GetFileAttributesW(L"config.json");
    if (fileAttr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        CreateDefaultConfig();
    }

    LoadSettings();
    // 第一次加载配置
    if (!ParseTags()) {
        MessageBoxW(NULL, L"无法读取或解析 config.json 文件。", L"错误", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        return 1;
    }

    // --- 修改：自动检测并修复重复标签 (静默) ---
    int renamedCount = FixDuplicateTags();
    if (renamedCount > 0) {
        // --- 移除了提示框 ---
        
        // 必须重新加载配置，因为 FixDuplicateTags 已经修改了 config.json
        if (!ParseTags()) {
            MessageBoxW(NULL, L"自动修复后无法重新加载 config.json。", L"错误", MB_OK | MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            if (g_hFont) DeleteObject(g_hFont);
            if (hLogFont) DeleteObject(hLogFont);
            return 1;
        }
    } else if (renamedCount == -1) {
        // 保留错误提示，因为这可能是文件权限问题
        MessageBoxW(NULL, L"尝试自动修复重复标签时发生错误。\n请检查 config.json 文件权限。", L"修复错误", MB_OK | MB_ICONWARNING);
    }
    // --- 修改结束 ---


    const wchar_t* CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        MessageBoxW(NULL, L"加载资源图标 (ID: 1) 失败！\n\n请检查：\n1. 资源ID是否为1\n2. 资源类型是否为ICON\n3. .ico文件是否有效且包含16x16尺寸\n\n程序将尝试使用默认图标。", L"资源加载错误", MB_OK | MB_ICONWARNING);
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        return 1;
    }

    if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) {
        if (!RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk)) {
            MessageBoxW(NULL, L"注册全局快捷键失败！\n可能已被其他程序占用。", L"快捷键错误", MB_OK | MB_ICONWARNING);
        }
    }

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon;
    wcsncpy(nid.szTip, L"程序正在运行...", ARRAYSIZE(nid.szTip) - 1);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = L'\0';

    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    // 确保启动前 g_isExiting 为 false
    g_isExiting = FALSE;
    StartSingBox();
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // --- 新增：检查是否是日志窗口的消息 ---
        // IsDialogMessage 允许在日志窗口的 EDIT 控件中使用 TAB 键
        if (hLogViewerWnd == NULL || !IsDialogMessageW(hLogViewerWnd, &msg)) {
             TranslateMessage(&msg);
             DispatchMessage(&msg);
        }
        // --- 新增结束 ---
    }
    
    // 程序退出前最后一次清理
    if (!g_isExiting) {
         g_isExiting = TRUE; // 确保在 GetMessage 循环外退出时也标记
         StopSingBox(); 
    }
    
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    CleanupDynamicNodes();
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    
    // --- 新增：清理字体 ---
    if (hLogFont) DeleteObject(hLogFont);
    // --- 新增结束 ---
    
    if (g_hFont) DeleteObject(g_hFont);
    return (int)msg.wParam;
}