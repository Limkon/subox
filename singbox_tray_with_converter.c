#define UNICODE
#define _UNICODE

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
#include <time.h>

// 确保 cJSON.c 在同一目录下，或者在编译命令中包含它
#include "cJSON.c"

#ifndef NIF_GUID
#define NIF_GUID 0x00000020
#endif

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

static const GUID APP_GUID = { 0xbfd8a583, 0x662a, 0x4fe3, { 0x97, 0x84, 0xfa, 0xb7, 0x8a, 0x33, 0x86, 0xa3 } };

#define WM_TRAY (WM_USER + 1)
#define WM_SINGBOX_CRASHED (WM_USER + 2)
#define WM_SINGBOX_RECONNECT (WM_USER + 3)
#define WM_LOG_UPDATE (WM_USER + 4)
#define WM_INIT_COMPLETE (WM_USER + 5)
#define WM_SHOW_TRAY_TIP (WM_USER + 6)

#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003
#define ID_TRAY_SETTINGS 1005
#define ID_TRAY_MANAGE_NODES 1006
#define ID_TRAY_SHOW_CONSOLE 1007
#define ID_TRAY_NODE_BASE 2000

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

#define ID_MODIFY_EDIT_CONTENT 4001
#define ID_MODIFY_OK_BTN 4002
#define ID_MODIFY_CANCEL_BTN 4003
#define ID_MODIFY_FORMAT_BTN 4004

#define ID_ADD_EDIT_CONTENT 5001
#define ID_ADD_OK_BTN 5002
#define ID_ADD_CANCEL_BTN 5003
#define ID_ADD_FORMAT_BTN 5004

#define ID_LOGVIEWER_EDIT 6001

#define ID_GLOBAL_HOTKEY 9001
#define ID_HOTKEY_CTRL 101

NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi = {0};
HFONT g_hFont = NULL;

wchar_t** nodeTags = NULL;
int nodeCount = 0;
int nodeCapacity = 0;
wchar_t currentNode[64] = L"";
int httpPort = 0;
int socksPort = 0;
int apiPort = 0; // [新增] 用于存储 API 端口

const wchar_t* REG_PATH_PROXY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

BOOL g_isIconVisible = TRUE;
UINT g_hotkeyModifiers = 0;
UINT g_hotkeyVk = 0;
wchar_t g_iniFilePath[MAX_PATH] = {0};
wchar_t g_configUrl[2048] = {0};

HANDLE hMonitorThread = NULL;
HANDLE hLogMonitorThread = NULL;
HANDLE hChildStd_OUT_Rd_Global = NULL;
volatile BOOL g_isExiting = FALSE; 

HWND hLogViewerWnd = NULL;
HFONT hLogFont = NULL;

typedef struct {
    wchar_t oldTag[256];
    wchar_t newTag[256];
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
int GetSocksInboundPort();
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
char* ConvertLfToCrlf(const char* input);
void CreateDefaultConfig();
BOOL WriteBufferToFileW(const wchar_t* filename, const char* buffer, long fileSize);
BOOL MoveFileCrossVolumeW(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName);
BOOL DownloadConfig(HWND hWndMain, const wchar_t* url, const wchar_t* savePath);
void PostTrayTip(HWND hWndMain, const wchar_t* title, const wchar_t* message);

// [新增] API 相关函数声明
BOOL ReloadSingBoxConfig(const wchar_t* configPath);
BOOL SendApiRequest(const wchar_t* method, const wchar_t* path, const char* jsonBody);

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
int FixDuplicateTags();

void OpenLogViewerWindow();
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ShowTrayTip(const wchar_t* title, const wchar_t* message) {
    if (!g_isIconVisible) {
        return;
    }
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
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) { 
        *fileSize = 0;
        return FALSE; 
    }
    fseek(f, 0, SEEK_END);
    *fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*fileSize <= 0) { 
        *fileSize = 0;
        *buffer = NULL;
        fclose(f); 
        return FALSE;
    }
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

void LoadSettings() {
    g_hotkeyModifiers = GetPrivateProfileIntW(L"Settings", L"Modifiers", 0, g_iniFilePath);
    g_hotkeyVk = GetPrivateProfileIntW(L"Settings", L"VK", 0, g_iniFilePath);
    g_isIconVisible = GetPrivateProfileIntW(L"Settings", L"ShowIcon", 1, g_iniFilePath);
    GetPrivateProfileStringW(L"Settings", L"ConfigUrl", L"", g_configUrl, ARRAYSIZE(g_configUrl), g_iniFilePath);
}

void SaveSettings() {
    wchar_t buffer[16];
    wsprintfW(buffer, L"%u", g_hotkeyModifiers);
    WritePrivateProfileStringW(L"Settings", L"Modifiers", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%u", g_hotkeyVk);
    WritePrivateProfileStringW(L"Settings", L"VK", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%d", g_isIconVisible);
    WritePrivateProfileStringW(L"Settings", L"ShowIcon", buffer, g_iniFilePath);
    WritePrivateProfileStringW(L"Settings", L"ConfigUrl", g_configUrl, g_iniFilePath);
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
    if (mods & MOD_WIN) flags |= MOD_WIN;
    return flags;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hHotkey, hLabel, hOkBtn, hCancelBtn;
    switch (msg) {
        case WM_CREATE: {
            hLabel = CreateWindowW(L"STATIC", L"显示/隐藏托盘图标快捷键:", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, NULL, NULL);
            hHotkey = CreateWindowExW(0, HOTKEY_CLASSW, NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 45, 240, 25, hWnd, (HMENU)ID_HOTKEY_CTRL, NULL, NULL);
            SendMessageW(hHotkey, HKM_SETHOTKEY, MAKEWORD(g_hotkeyVk, ModToHotkeyf(g_hotkeyModifiers)), 0);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 60, 85, 80, 25, hWnd, (HMENU)IDOK, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 160, 85, 80, 25, hWnd, (HMENU)IDCANCEL, NULL, NULL);
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
    HWND hSettingsWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS_NAME, L"隐藏图标", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300, 160, hwnd, NULL, wc.hInstance, NULL);
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

BOOL ParseTags() {
    CleanupDynamicNodes();
    currentNode[0] = L'\0';
    httpPort = 0;
    socksPort = 0;
    apiPort = 0; // 重置 API 端口
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

    // 解析 API 端口
    cJSON* experimental = cJSON_GetObjectItem(root, "experimental");
    if (experimental) {
        cJSON* clash_api = cJSON_GetObjectItem(experimental, "clash_api");
        if (clash_api) {
            cJSON* ext_ctrl = cJSON_GetObjectItem(clash_api, "external_controller");
            if (cJSON_IsString(ext_ctrl) && ext_ctrl->valuestring) {
                const char* portStr = strrchr(ext_ctrl->valuestring, ':');
                if (portStr) {
                    apiPort = atoi(portStr + 1);
                }
            }
        }
    }

    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    cJSON* outbound = NULL;
    cJSON_ArrayForEach(outbound, outbounds) {
        cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
        cJSON* type = cJSON_GetObjectItem(outbound, "type"); // 获取节点类型

        // [修改] 增加过滤逻辑：如果类型是 "dns"，则跳过，不添加到列表中
        if (cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "dns") == 0) {
            continue;
        }

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
    
    // 获取当前选中的节点 (route -> final)
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (cJSON_IsString(final_outbound) && final_outbound->valuestring) {
            MultiByteToWideChar(CP_UTF8, 0, final_outbound->valuestring, -1, currentNode, ARRAYSIZE(currentNode));
        }
    }

    // 解析入站端口
    cJSON* inbounds = cJSON_GetObjectItem(root, "inbounds");
    cJSON* inbound = NULL;
    cJSON_ArrayForEach(inbound, inbounds) {
        cJSON* type_item = cJSON_GetObjectItem(inbound, "type");
        if (!cJSON_IsString(type_item)) continue;
        const char* type_str = type_item->valuestring;
        cJSON* listenPort_item = cJSON_GetObjectItem(inbound, "listen_port");
        if (!cJSON_IsNumber(listenPort_item)) continue;
        int port = listenPort_item->valueint;
        if (httpPort == 0 && (strcmp(type_str, "http") == 0 || strcmp(type_str, "mixed") == 0)) {
            httpPort = port;
        }
        if (socksPort == 0 && (strcmp(type_str, "socks") == 0 || strcmp(type_str, "mixed") == 0)) {
            socksPort = port;
        }
        if (httpPort != 0 && socksPort != 0) {
            break;
        }
    }
    cJSON_Delete(root);
    free(buffer);
    return TRUE;
}

int GetHttpInboundPort() {
    return httpPort;
}

int GetSocksInboundPort() {
    return socksPort;
}

DWORD WINAPI MonitorThread(LPVOID lpParam) {
    HANDLE hProcess = (HANDLE)lpParam;
    WaitForSingleObject(hProcess, INFINITE);
    if (!g_isExiting) {
        PostMessageW(hwnd, WM_SINGBOX_CRASHED, 0, 0);
    }
    return 0;
}

DWORD WINAPI LogMonitorThread(LPVOID lpParam) {
    char readBuf[4096];
    char lineBuf[8192] = {0};
    DWORD dwRead;
    BOOL bSuccess;
    static time_t lastLogTriggeredRestart = 0;
    const time_t RESTART_COOLDOWN = 60;
    HANDLE hPipe = (HANDLE)lpParam;

    while (TRUE) {
        bSuccess = ReadFile(hPipe, readBuf, sizeof(readBuf) - 1, &dwRead, NULL);
        if (!bSuccess || dwRead == 0) {
            break;
        }
        readBuf[dwRead] = '\0';
        
        if (!g_isExiting && hLogViewerWnd != NULL && IsWindow(hLogViewerWnd)) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, NULL, 0);
            if (wideLen > 0) {
                wchar_t* pWideBuf = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                if (pWideBuf) {
                    MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, pWideBuf, wideLen);
                    if (!PostMessageW(hLogViewerWnd, WM_LOG_UPDATE, 0, (LPARAM)pWideBuf)) {
                        free(pWideBuf);
                    }
                }
            }
        }
        
        strncat(lineBuf, readBuf, sizeof(lineBuf) - strlen(lineBuf) - 1);
        if (g_isExiting) {
            continue;
        }
        char* fatal_pos = strstr(lineBuf, "level\"=\"fatal");
        char* dial_pos = strstr(lineBuf, "failed to dial");
        if (fatal_pos != NULL || dial_pos != NULL) {
            time_t now = time(NULL);
            if (now - lastLogTriggeredRestart > RESTART_COOLDOWN) {
                lastLogTriggeredRestart = now;
                PostMessageW(hwnd, WM_SINGBOX_RECONNECT, 0, 0);
            }
            lineBuf[0] = '\0';
        } else {
            char* last_newline = strrchr(lineBuf, '\n');
            if (last_newline != NULL) {
                strcpy(lineBuf, last_newline + 1);
            } else if (strlen(lineBuf) > 4096) {
                lineBuf[0] = '\0';
            }
        }
    }
    CloseHandle(hPipe);
    return 0;
}

void StartSingBox() {
    // =======================================================================
    // [修复] 设置环境变量以兼容旧版 GeoIP/GeoSite 配置
    // 新版 Sing-box 默认禁用了 .db 格式，需要显式开启兼容模式，否则会报错 FATAL 退出
    SetEnvironmentVariableW(L"ENABLE_DEPRECATED_GEOIP", L"true");
    SetEnvironmentVariableW(L"ENABLE_DEPRECATED_GEOSITE", L"true"); // 预防性添加，防止 GeoSite 也报错
    // =======================================================================

    HANDLE hPipe_Rd_Local = NULL;
    HANDLE hPipe_Wr_Local = NULL;
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hPipe_Rd_Local, &hPipe_Wr_Local, &sa, 0)) {
        ShowError(L"管道创建失败", L"无法为核心程序创建输出管道。");
        return;
    }
    if (!SetHandleInformation(hPipe_Rd_Local, HANDLE_FLAG_INHERIT, 0)) {
        ShowError(L"管道句柄属性设置失败", L"无法设置输出管道读取句柄的属性。");
        CloseHandle(hPipe_Rd_Local);
        CloseHandle(hPipe_Wr_Local);
        return;
    }

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
        CloseHandle(hChildStd_OUT_Rd_Global);
        hChildStd_OUT_Rd_Global = NULL;
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    CloseHandle(hPipe_Wr_Local);
    if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) {
        char chBuf[4096] = {0};
        DWORD dwRead = 0;
        wchar_t errorOutput[4096] = L"";
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
        CloseHandle(hChildStd_OUT_Rd_Global);
        hChildStd_OUT_Rd_Global = NULL;
    } 
    else {
        hMonitorThread = CreateThread(NULL, 0, MonitorThread, pi.hProcess, 0, NULL);
        HANDLE hPipeForLogThread;
        if (DuplicateHandle(GetCurrentProcess(), hChildStd_OUT_Rd_Global,
                           GetCurrentProcess(), &hPipeForLogThread, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        {
            hLogMonitorThread = CreateThread(NULL, 0, LogMonitorThread, hPipeForLogThread, 0, NULL);
        }
    }
}

// [新增] 通用 HTTP API 请求发送函数
BOOL SendApiRequest(const wchar_t* method, const wchar_t* path, const char* jsonBody) {
    if (apiPort <= 0) return FALSE;

    HINTERNET hInternet = InternetOpenW(L"SingBoxTray", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) return FALSE;

    HINTERNET hConnect = InternetConnectW(hInternet, L"127.0.0.1", (INTERNET_PORT)apiPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    HINTERNET hRequest = HttpOpenRequestW(hConnect, method, path, NULL, NULL, NULL, 0, 0);
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    const wchar_t* headers = L"Content-Type: application/json";
    BOOL result = HttpSendRequestW(hRequest, headers, -1, (LPVOID)jsonBody, (DWORD)(jsonBody ? strlen(jsonBody) : 0));
    
    // 检查 HTTP 状态码
    if (result) {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &size, NULL);
        if (statusCode < 200 || statusCode >= 300) {
            result = FALSE;
        }
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    return result;
}

// [新增] 通过 API 重载配置
BOOL ReloadSingBoxConfig(const wchar_t* configPath) {
    // 获取 config.json 的绝对路径
    wchar_t absPath[MAX_PATH];
    if (GetFullPathNameW(configPath, MAX_PATH, absPath, NULL) == 0) {
        return FALSE;
    }

    // 将宽字符路径转换为 JSON 字符串需要的格式（UTF-8，转义反斜杠）
    char utf8Path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, absPath, -1, utf8Path, sizeof(utf8Path), NULL, NULL);

    // 构建 JSON Body: {"path": "C:\\path\\to\\config.json"}
    // 需要对路径中的 \ 进行转义
    char escapedPath[MAX_PATH * 4] = {0};
    int j = 0;
    for (int i = 0; utf8Path[i] != '\0'; i++) {
        if (utf8Path[i] == '\\') {
            escapedPath[j++] = '\\';
            escapedPath[j++] = '\\';
        } else {
            escapedPath[j++] = utf8Path[i];
        }
    }
    escapedPath[j] = '\0';

    char jsonBody[MAX_PATH * 5];
    snprintf(jsonBody, sizeof(jsonBody), "{\"path\": \"%s\"}", escapedPath);

    // 调用 Clash API 的 reload 接口
    // endpoint: PUT /configs?force=false (force=false 允许增量更新或平滑重载)
    return SendApiRequest(L"PUT", L"/configs?force=false", jsonBody);
}

// [重构] 切换节点函数：尝试 API 热重载，失败则重启
void SwitchNode(const wchar_t* tag) {
    // 1. 始终修改磁盘上的 config.json 以确保下次启动生效
    SafeReplaceOutbound(tag);
    
    // 2. 更新内存中的当前节点记录
    wcsncpy(currentNode, tag, ARRAYSIZE(currentNode) - 1);
    currentNode[ARRAYSIZE(currentNode)-1] = L'\0';

    wchar_t message[256];
    
    // 3. 尝试 API 热重载
    BOOL apiSuccess = FALSE;
    if (apiPort > 0) {
        if (ReloadSingBoxConfig(L"config.json")) {
            apiSuccess = TRUE;
            wsprintfW(message, L"节点已切换: %s\n(API 热重载成功)", tag);
        }
    }

    // 4. 根据 API 结果决定是否重启进程
    if (apiSuccess) {
        ShowTrayTip(L"切换成功", message);
    } else {
        // API 失败或未配置 API，回退到重启进程的方式
        g_isExiting = TRUE;
        StopSingBox();
        g_isExiting = FALSE;
        StartSingBox();
        
        wsprintfW(message, L"当前节点: %s", tag);
        ShowTrayTip(L"切换成功", message);
    }
}

void SetSystemProxy(BOOL enable) {
    int hPort = GetHttpInboundPort();
    int sPort = GetSocksInboundPort();
    if (hPort == 0 && sPort == 0 && enable) {
        MessageBoxW(NULL, L"未找到 HTTP 或 SOCKS (或 Mixed) 入站端口，无法设置系统代理。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t proxyServerString[256] = {0};
    wchar_t proxyBypassString[64] = {0};
    if (enable) {
        if (hPort > 0) {
            wchar_t httpBuf[128];
            wsprintfW(httpBuf, L"http=127.0.0.1:%d", hPort);
            wcsncat(proxyServerString, httpBuf, ARRAYSIZE(proxyServerString) - 1);
        }
        if (sPort > 0) {
            wchar_t socksBuf[128];
            wsprintfW(socksBuf, L"socks=127.0.0.1:%d", sPort);
            if (proxyServerString[0] != L'\0') {
                wcsncat(proxyServerString, L";", ARRAYSIZE(proxyServerString) - wcslen(proxyServerString) - 1);
            }
            wcsncat(proxyServerString, socksBuf, ARRAYSIZE(proxyServerString) - wcslen(proxyServerString) - 1);
        }
        wcsncpy(proxyBypassString, L"<local>", ARRAYSIZE(proxyBypassString) - 1);
    }
    if (IsWindows8OrGreater()) {
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
            ShowError(L"代理设置失败", L"无法打开注册表键。");
            return;
        }
        if (enable) {
            DWORD dwEnable = 1;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
            RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, (const BYTE*)proxyBypassString, (wcslen(proxyBypassString) + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)proxyServerString, (wcslen(proxyServerString) + 1) * sizeof(wchar_t));
            RegDeleteValueW(hKey, L"SocksProxyServer"); 
        } else {
            DWORD dwEnable = 0;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
            RegDeleteValueW(hKey, L"SocksProxyServer");
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
            options[0].Value.dwValue = PROXY_TYPE_PROXY;
            options[1].Value.pszValue = proxyServerString;
            options[2].Value.pszValue = proxyBypassString;
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
    wchar_t proxyServer[1024] = {0};
    DWORD dwProxySize = sizeof(proxyServer);
    int hPort = GetHttpInboundPort();
    int sPort = GetSocksInboundPort();
    BOOL isEnabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&dwEnable, &dwSize) == ERROR_SUCCESS) {
            if (dwEnable == 1) {
                if (RegQueryValueExW(hKey, L"ProxyServer", NULL, NULL, (LPBYTE)proxyServer, &dwProxySize) == ERROR_SUCCESS) {
                    BOOL httpMatch = FALSE;
                    BOOL socksMatch = FALSE;
                    if (hPort > 0) {
                        wchar_t expectedHttp[128];
                        wsprintfW(expectedHttp, L"http=127.0.0.1:%d", hPort);
                        if (wcsstr(proxyServer, expectedHttp) != NULL) {
                            httpMatch = TRUE;
                        } else {
                            wchar_t expectedHttpLegacy[64];
                            wsprintfW(expectedHttpLegacy, L"127.0.0.1:%d", hPort);
                            if (wcscmp(proxyServer, expectedHttpLegacy) == 0) {
                                httpMatch = TRUE;
                            }
                        }
                    }
                    if (sPort > 0) {
                         wchar_t expectedSocks[128];
                         wsprintfW(expectedSocks, L"socks=127.0.0.1:%d", sPort);
                         if (wcsstr(proxyServer, expectedSocks) != NULL) {
                            socksMatch = TRUE;
                         }
                    }
                    if (hPort > 0 && sPort > 0 && hPort == sPort) {
                        if (httpMatch || socksMatch) isEnabled = TRUE;
                    } 
                    else {
                        if (hPort > 0 && httpMatch) isEnabled = TRUE;
                        if (sPort > 0 && socksMatch) isEnabled = TRUE;
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return isEnabled;
}

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
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (final_outbound) {
            cJSON_SetValuestring(final_outbound, newTagMb);
        } else {
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
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"隐藏图标");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"显示日志");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static time_t lastAutoRestart = 0;
    const time_t RESTART_COOLDOWN = 60;

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
            g_isExiting = TRUE;
            if (hLogViewerWnd != NULL) {
                DestroyWindow(hLogViewerWnd);
            }
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
        } else if (id == ID_TRAY_SETTINGS) {
            OpenSettingsWindow();
        } else if (id == ID_TRAY_MANAGE_NODES) {
            OpenNodeManagerWindow();
        } else if (id == ID_TRAY_SHOW_CONSOLE) {
            OpenLogViewerWindow();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    } else if (msg == WM_HOTKEY) {
        if (wParam == ID_GLOBAL_HOTKEY) {
            ToggleTrayIconVisibility();
        }
    }
    else if (msg == WM_SINGBOX_CRASHED) {
        ShowTrayTip(L"Sing-box 监控", L"核心进程意外终止。请手动检查。");
    }
    else if (msg == WM_SINGBOX_RECONNECT) {
        static time_t lastErrorNotify = 0; 
        const time_t NOTIFY_COOLDOWN = 60;
        time_t now = time(NULL);
        if (now - lastErrorNotify > NOTIFY_COOLDOWN) {
            lastErrorNotify = now;
            ShowTrayTip(L"Sing-box 监控", L"检测到核心日志严重错误 (fatal 或 dial failed)。");
        }
    }
    else if (msg == WM_INIT_COMPLETE) {
        BOOL success = (BOOL)wParam;
        if (success) {
            ParseTags();
            wcsncpy(nid.szTip, L"程序正在运行...", ARRAYSIZE(nid.szTip) - 1);
            if(g_isIconVisible) { Shell_NotifyIconW(NIM_MODIFY, &nid); }
            ShowTrayTip(L"启动成功", L"程序已准备就绪。");
        } else {
            ShowTrayTip(L"启动失败", L"核心初始化失败，程序将退出。");
            PostMessageW(hWnd, WM_COMMAND, ID_TRAY_EXIT, 0);
        }
    }
    else if (msg == WM_SHOW_TRAY_TIP) {
        wchar_t* pTitle = (wchar_t*)wParam;
        wchar_t* pMessage = (wchar_t*)lParam;
        if (pTitle && pMessage) {
            ShowTrayTip(pTitle, pMessage);
            free(pTitle);
            free(pMessage);
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void StopSingBox() {
    g_isExiting = TRUE; 
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
    if (hMonitorThread) {
        WaitForSingleObject(hMonitorThread, 1000);
        CloseHandle(hMonitorThread);
    }
    if (hChildStd_OUT_Rd_Global) {
        CloseHandle(hChildStd_OUT_Rd_Global);
    }
    if (hLogMonitorThread) {
        WaitForSingleObject(hLogMonitorThread, 1000);
        CloseHandle(hLogMonitorThread);
    }
    ZeroMemory(&pi, sizeof(pi));
    hMonitorThread = NULL;
    hLogMonitorThread = NULL;
    hChildStd_OUT_Rd_Global = NULL;
}

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

void CreateDefaultConfig() {
    // [更新] 添加 experimental 字段以支持 API 热重载
    const char* defaultConfig =
        "{\n"
        "\t\"log\": {\n"
        "\t\t\"disabled\": false,\n"
        "\t\t\"level\": \"debug\"\n"
        "\t},\n"
        "\t\"experimental\": {\n"
        "\t\t\"clash_api\": {\n"
        "\t\t\t\"external_controller\": \"127.0.0.1:9090\",\n"
        "\t\t\t\"external_ui\": \"ui\",\n"
        "\t\t\t\"secret\": \"\",\n"
        "\t\t\t\"external_ui_download_url\": \"\",\n"
        "\t\t\t\"external_ui_download_detour\": \"\"\n"
        "\t\t}\n"
        "\t},\n"
        "\t\"dns\": {\n"
        "\t\t\"servers\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns_resolver-tx\",\n"
        "\t\t\t\t\"address\": \"119.29.29.29\",\n"
        "\t\t\t\t\"detour\": \"🎯 全球直连\"\n"
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-direct-tx\",\n"
        "\t\t\t\t\"address\": \"https://doh.pub/dns-query\",\n"
        "\t\t\t\t\"address_resolver\": \"dns_resolver-tx\",\n"
        "\t\t\t\t\"detour\": \"🎯 全球直连\"\n"
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-proxy-cf\",\n"
        "\t\t\t\t\"address\": \"https://cloudflare-dns.com/dns-query\",\n"
        "\t\t\t\t\"address_resolver\": \"dns_resolver-tx\",\n"
        "\t\t\t\t\"detour\": \"🎈 自动选择\"\n"
        "\t\t\t},\n"
        "\t\t\t{\n"
        "\t\t\t\t\"tag\": \"dns-block\",\n"
        "\t\t\t\t\"address\": \"rcode://refused\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"rules\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"domain_suffix\": [\n"
        "\t\t\t\t\t\"visa.com.tw\",\n"
        "\t\t\t\t\t\"visa.com.sg\",\n"
        "\t\t\t\t\t\"visa.com\",\n"
        "\t\t\t\t\t\"abrdns.com\"\n"
        "\t\t\t\t],\n"
        "\t\t\t\t\"server\": \"dns-direct-tx\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"strategy\": \"ipv4_only\",\n"
        "\t\t\"final\": \"dns-proxy-cf\"\n"
        "\t},\n"
        "\t\"inbounds\": [\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"http-in\",\n"
        "\t\t\t\"type\": \"http\",\n"
        "\t\t\t\"listen\": \"127.0.0.1\",\n"
        "\t\t\t\"listen_port\": 10809\n"
        "\t\t}\n"
        "\t],\n"
        "\t\"outbounds\": [\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"🎈 自动选择\",\n"
        "\t\t\t\"type\": \"urltest\",\n"
        "\t\t\t\"outbounds\": [\n"
        "\t\t\t\t\"SEA\"\n"
        "\t\t\t],\n"
        "\t\t\t\"url\": \"http://www.gstatic.com/generate_204\",\n"
        "\t\t\t\"interval\": \"10m\",\n"
        "\t\t\t\"tolerance\": 50\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"🎯 全球直连\",\n"
        "\t\t\t\"type\": \"direct\"\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"tag\": \"🚫 断开连接\",\n"
        "\t\t\t\"type\": \"block\"\n"
        "\t\t},\n"
        "\t\t{\n"
        "\t\t\t\"type\": \"vless\",\n"
        "\t\t\t\"tag\": \"xxx\",\n"
        "\t\t\t\"server\": \"xxx.xxx.xxx.xxx\",\n"
        "\t\t\t\"server_port\": 443,\n"
        "\t\t\t\"uuid\": \"xxx-xxx-xxx-bb87-b06f9ddc5e89\",\n"
        "\t\t\t\"flow\": \"\",\n"
        "\t\t\t\"tls\": {\n"
        "\t\t\t\t\"enabled\": true,\n"
        "\t\t\t\t\"server_name\": \"xxx.xxx.xxx\"\n"
        "\t\t\t},\n"
        "\t\t\t\"transport\": {\n"
        "\t\t\t\t\"type\": \"ws\",\n"
        "\t\t\t\t\"path\": \"/?ed=2560\",\n"
        "\t\t\t\t\"headers\": {\n"
        "\t\t\t\t\t\"Host\": \"xxx.xxx.xxx\"\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t],\n"
        "\t\"route\": {\n"
        "\t\t\"rules\": [\n"
        "\t\t\t{\n"
        "\t\t\t\t\"ip_cidr\": [\n"
        "\t\t\t\t\t\"119.29.29.29\",\n"
        "\t\t\t\t\t\"120.53.53.53\"\n"
        "\t\t\t\t],\n"
        "\t\t\t\t\"outbound\": \"🎯 全球直连\"\n"
        "\t\t\t}\n"
        "\t\t],\n"
        "\t\t\"final\": \"SEA\",\n"
        "\t\t\"auto_detect_interface\": true,\n"
        "\t\t\"find_process\": true\n"
        "\t}\n"
        "}";

    FILE* f = NULL;
    if (_wfopen_s(&f, L"config.json", L"wb") == 0 && f != NULL) {
        fwrite(defaultConfig, 1, strlen(defaultConfig), f);
        fclose(f);
        MessageBoxW(NULL,
            L"未找到 config.json，已为您生成默认配置文件。\n\n"
            L"请在使用前修改 config.json 中的 'xxx' 节点信息。", 
            L"提示", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(NULL, L"无法创建默认的 config.json 文件。", L"错误", MB_OK | MB_ICONERROR);
    }
}

BOOL WriteBufferToFileW(const wchar_t* filename, const char* buffer, long fileSize) {
    if (!buffer || fileSize <= 0) {
        return FALSE;
    }
    FILE* f = NULL;
    if (_wfopen_s(&f, filename, L"wb") != 0 || !f) {
        return FALSE;
    }
    size_t written = fwrite(buffer, 1, fileSize, f);
    fclose(f);
    return (written == fileSize);
}

BOOL MoveFileCrossVolumeW(const wchar_t* lpExistingFileName, const wchar_t* lpNewFileName) {
    if (MoveFileExW(lpExistingFileName, lpNewFileName, MOVEFILE_REPLACE_EXISTING)) {
        return TRUE;
    }
    if (GetLastError() == ERROR_NOT_SAME_DEVICE) {
        char* buffer = NULL;
        long size = 0;
        if (!ReadFileToBuffer(lpExistingFileName, &buffer, &size) || size == 0) {
            if (buffer) free(buffer);
            return FALSE;
        }
        BOOL writeSuccess = WriteBufferToFileW(lpNewFileName, buffer, size);
        free(buffer);
        if (!writeSuccess) {
            return FALSE;
        }
        DeleteFileW(lpExistingFileName);
        return TRUE;
    }
    return FALSE;
}

void PostTrayTip(HWND hWndMain, const wchar_t* title, const wchar_t* message) {
    if (g_isExiting || !IsWindow(hWndMain)) {
        return;
    }

    size_t titleLen = wcslen(title) + 1;
    size_t msgLen = wcslen(message) + 1;
    wchar_t* pTitle = (wchar_t*)malloc(titleLen * sizeof(wchar_t));
    wchar_t* pMessage = (wchar_t*)malloc(msgLen * sizeof(wchar_t));
    if (!pTitle || !pMessage) {
        if (pTitle) free(pTitle);
        if (pMessage) free(pMessage);
        return;
    }
    wcsncpy(pTitle, title, titleLen);
    pTitle[titleLen - 1] = L'\0';
    wcsncpy(pMessage, message, msgLen);
    pMessage[msgLen - 1] = L'\0';
    
    if (!PostMessageW(hWndMain, WM_SHOW_TRAY_TIP, (WPARAM)pTitle, (LPARAM)pMessage)) {
        free(pTitle);
        free(pMessage);
    }
}

BOOL DownloadConfig(HWND hWndMain, const wchar_t* url, const wchar_t* savePath) {
    wchar_t cmdLine[4096];
    wchar_t fullSavePath[MAX_PATH];
    wchar_t fullCurlPath[MAX_PATH];
    wchar_t moduleDir[MAX_PATH];
    BOOL useSystemCurl = FALSE;

    GetModuleFileNameW(NULL, moduleDir, MAX_PATH);
    wchar_t* p = wcsrchr(moduleDir, L'\\');
    if (p) *p = L'\0'; else wcsncpy(moduleDir, L".", MAX_PATH);

    wsprintfW(fullCurlPath, L"%s\\curl.exe", moduleDir);
    DWORD fileAttr = GetFileAttributesW(fullCurlPath);
    if (fileAttr == INVALID_FILE_ATTRIBUTES || (fileAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        useSystemCurl = TRUE;
        wcsncpy(fullCurlPath, L"curl", MAX_PATH);
    }

    if (GetFullPathNameW(savePath, MAX_PATH, fullSavePath, NULL) == 0) {
        PostTrayTip(hWndMain, L"路径错误", L"无法获取保存路径。");
        return FALSE;
    }

    const wchar_t* userAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    
    if (useSystemCurl) {
        wsprintfW(cmdLine, 
            L"curl -ksSL -A \"%s\" --connect-timeout 10 --max-time 30 -o \"%s\" \"%s\"", 
            userAgent, fullSavePath, url
        );
    } else {
        wsprintfW(cmdLine, 
            L"\"%s\" -ksSL -A \"%s\" --connect-timeout 10 --max-time 30 -o \"%s\" \"%s\"", 
            fullCurlPath, userAgent, fullSavePath, url
        );
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION downloaderPi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, moduleDir, &si, &downloaderPi)) {
        if (useSystemCurl) {
            PostTrayTip(hWndMain, L"下载组件缺失", L"未找到 curl.exe。请下载 curl.exe 放入程序目录，或更新 Windows 系统。");
        } else {
            PostTrayTip(hWndMain, L"启动失败", L"无法启动 curl 进程。");
        }
        return FALSE;
    }

    DWORD waitResult = WaitForSingleObject(downloaderPi.hProcess, 35000); 

    if (waitResult == WAIT_TIMEOUT) {
        PostTrayTip(hWndMain, L"下载超时", L"连接服务器超时，请检查网络或 URL。");
        TerminateProcess(downloaderPi.hProcess, 1);
        CloseHandle(downloaderPi.hProcess);
        CloseHandle(downloaderPi.hThread);
        return FALSE;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(downloaderPi.hProcess, &exitCode);
    CloseHandle(downloaderPi.hProcess);
    CloseHandle(downloaderPi.hThread);

    if (exitCode != 0) {
        wchar_t errorMsg[128];
        wsprintfW(errorMsg, L"下载失败 (代码: %lu)。请检查 URL 是否有效。", exitCode);
        PostTrayTip(hWndMain, L"下载错误", errorMsg);
        return FALSE;
    }

    long fileSize = 0;
    char* fileBuffer = NULL;
    if (ReadFileToBuffer(savePath, &fileBuffer, &fileSize)) {
        if (fileSize < 50) {
             PostTrayTip(hWndMain, L"下载失败", L"下载的内容无效 (空白或过短)。可能链接已过期。");
             free(fileBuffer);
             DeleteFileW(savePath);
             return FALSE;
        }
        free(fileBuffer);
        return TRUE; 
    }

    return FALSE;
}

void RefreshNodeListBox(HWND hListBox) {
    SendMessageW(hListBox, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < nodeCount; i++) {
        SendMessageW(hListBox, LB_ADDSTRING, 0, (LPARAM)nodeTags[i]);
    }
}

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

LRESULT CALLBACK NodeManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hListBox, hModifyBtn, hDeleteBtn, hAddBtn, hInfoLabel;

    switch (msg) {
        case WM_CREATE: {
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
                            // [重构] 尝试热重载
                            BOOL reloaded = FALSE;
                            if (apiPort > 0) {
                                 if (ReloadSingBoxConfig(L"config.json")) {
                                     reloaded = TRUE;
                                     MessageBoxW(hWnd, L"当前节点已修改，核心已通过 API 热重载配置。", L"提示", MB_OK | MB_ICONINFORMATION);
                                 }
                            }
                            
                            if (!reloaded) {
                                MessageBoxW(hWnd, L"检测到当前活动节点已被修改，核心将自动重启以应用更改。", L"提示", MB_OK | MB_ICONINFORMATION);
                                g_isExiting = TRUE;
                                StopSingBox();
                                g_isExiting = FALSE;
                                StartSingBox();
                            }
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

LRESULT CALLBACK ModifyNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit, hOkBtn, hCancelBtn, hFormatBtn, hLabel;

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
            MODIFY_NODE_PARAMS* pParams = (MODIFY_NODE_PARAMS*)pCreate->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pParams);

            hLabel = CreateWindowW(L"STATIC", L"节点内容 (JSON格式):", WS_CHILD | WS_VISIBLE, 15, 10, 200, 20, hWnd, NULL, NULL, NULL);
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 15, 35, 450, 280, hWnd, (HMENU)ID_MODIFY_EDIT_CONTENT, NULL, NULL);
            hFormatBtn = CreateWindowW(L"BUTTON", L"JSON格式化", WS_CHILD | WS_VISIBLE, 60, 340, 100, 30, hWnd, (HMENU)ID_MODIFY_FORMAT_BTN, NULL, NULL);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 340, 80, 30, hWnd, (HMENU)ID_MODIFY_OK_BTN, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 360, 340, 80, 30, hWnd, (HMENU)ID_MODIFY_CANCEL_BTN, NULL, NULL);

            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hFormatBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            HFONT hJsonFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
            if(hJsonFont) {
                SendMessage(hEdit, WM_SETFONT, (WPARAM)hJsonFont, TRUE);
                SetPropW(hWnd, L"JsonFont", (HANDLE)hJsonFont);
            }

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
                    MODIFY_NODE_PARAMS* pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
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
                        pParams->success = TRUE;
                        DestroyWindow(hWnd);
                    } else {
                        pParams->success = FALSE;
                        MessageBoxW(hWnd, L"修改失败，请检查配置文件是否可写或格式是否正确。", L"错误", MB_OK | MB_ICONERROR);
                    }
                    free(newContentMb);
                    break;
                }
                case ID_MODIFY_CANCEL_BTN: {
                    MODIFY_NODE_PARAMS* pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                    if(pParams) pParams->success = FALSE;
                    DestroyWindow(hWnd);
                    break;
                }
            }
            break;
        }
        case WM_CLOSE: {
            MODIFY_NODE_PARAMS* pParams = (MODIFY_NODE_PARAMS*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if(pParams) pParams->success = FALSE;
            DestroyWindow(hWnd);
            break;
        }
        case WM_DESTROY: {
             HANDLE hFont = GetPropW(hWnd, L"JsonFont");
             if (hFont) {
                 DeleteObject((HFONT)hFont);
                 RemovePropW(hWnd, L"JsonFont");
             }
             break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK AddNodeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit, hOkBtn, hCancelBtn, hFormatBtn, hLabel;

    switch (msg) {
        case WM_CREATE: {
            hLabel = CreateWindowW(L"STATIC", L"新节点内容 (JSON格式):", WS_CHILD | WS_VISIBLE, 15, 10, 200, 20, hWnd, NULL, NULL, NULL);
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 15, 35, 450, 280, hWnd, (HMENU)ID_ADD_EDIT_CONTENT, NULL, NULL);
            hFormatBtn = CreateWindowW(L"BUTTON", L"JSON格式化", WS_CHILD | WS_VISIBLE, 60, 340, 100, 30, hWnd, (HMENU)ID_ADD_FORMAT_BTN, NULL, NULL);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 340, 80, 30, hWnd, (HMENU)ID_ADD_OK_BTN, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 360, 340, 80, 30, hWnd, (HMENU)ID_ADD_CANCEL_BTN, NULL, NULL);
            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hFormatBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            
            HFONT hJsonFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
            if(hJsonFont) {
                SendMessage(hEdit, WM_SETFONT, (WPARAM)hJsonFont, TRUE);
                SetPropW(hWnd, L"JsonFont", (HANDLE)hJsonFont);
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
             HANDLE hFont = GetPropW(hWnd, L"JsonFont");
             if (hFont) {
                 DeleteObject((HFONT)hFont);
                 RemovePropW(hWnd, L"JsonFont");
             }
             break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

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
    if (strcmp(tagToDeleteMb, "直接连接") != 0 &&
        strcmp(tagToDeleteMb, "阻塞") != 0 &&
        strcmp(tagToDeleteMb, "自动切换") != 0)
    {
        cJSON* outbound_iter = NULL;
        cJSON_ArrayForEach(outbound_iter, outbounds) {
            cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
            cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
            if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "🎈 自动选择") == 0 &&
                cJSON_IsString(type_item) && (strcmp(type_item->valuestring, "selector") == 0 || strcmp(type_item->valuestring, "urltest") == 0))
            {
                cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                if (cJSON_IsArray(selector_outbounds)) {
                    int i = 0;
                    cJSON* selector_tag_item = NULL;
                    cJSON_ArrayForEach(selector_tag_item, selector_outbounds) {
                        if (cJSON_IsString(selector_tag_item) && strcmp(selector_tag_item->valuestring, tagToDeleteMb) == 0) {
                            cJSON_DeleteItemFromArray(selector_outbounds, i);
                            break;
                        }
                        i++;
                    }
                }
                break;
            }
        }
    }
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
        cJSON_Delete(newNode);
    }
    if (success && wcscmp(oldTag, currentNode) == 0) {
        cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
        const char* newTagMb = newTagJson->valuestring;
        if (strcmp(oldTagMb, newTagMb) != 0) {
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
    if (success) {
        cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
        const char* newTagMb = newTagJson->valuestring;
        if (strcmp(oldTagMb, newTagMb) != 0) {
            if (strcmp(oldTagMb, "🎯 全球直连") != 0 &&
                strcmp(oldTagMb, "🚫 断开连接") != 0 &&
                strcmp(oldTagMb, "🎈 自动选择") != 0)
            {
                cJSON* outbound_iter = NULL;
                cJSON_ArrayForEach(outbound_iter, outbounds) {
                    cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
                    cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
                    if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "🎈 自动选择") == 0 &&
                        cJSON_IsString(type_item) && (strcmp(type_item->valuestring, "selector") == 0 || strcmp(type_item->valuestring, "urltest") == 0))
                    {
                        cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                        if (cJSON_IsArray(selector_outbounds)) {
                            cJSON* selector_tag_item = NULL;
                            cJSON_ArrayForEach(selector_tag_item, selector_outbounds) {
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
    cJSON* newTagJson = cJSON_GetObjectItem(newNode, "tag");
    if (!cJSON_IsString(newTagJson) || !newTagJson->valuestring || strlen(newTagJson->valuestring) == 0) {
        cJSON_Delete(newNode);
        cJSON_Delete(root);
        return FALSE;
    }
    const char* newTagMb = newTagJson->valuestring;
    BOOL success = FALSE;
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    if (cJSON_IsArray(outbounds)) {
        cJSON_AddItemToArray(outbounds, newNode);
        success = TRUE;
        if (strcmp(newTagMb, "🎯 全球直连") != 0 &&
            strcmp(newTagMb, "🚫 断开连接") != 0 &&
            strcmp(newTagMb, "🎈 自动选择") != 0)
        {
            cJSON* outbound_iter = NULL;
            cJSON_ArrayForEach(outbound_iter, outbounds) {
                cJSON* tag_item = cJSON_GetObjectItem(outbound_iter, "tag");
                cJSON* type_item = cJSON_GetObjectItem(outbound_iter, "type");
                if (cJSON_IsString(tag_item) && strcmp(tag_item->valuestring, "🎈 自动选择") == 0 &&
                    cJSON_IsString(type_item) && (strcmp(type_item->valuestring, "selector") == 0 || strcmp(type_item->valuestring, "urltest") == 0))
                {
                    cJSON* selector_outbounds = cJSON_GetObjectItem(outbound_iter, "outbounds");
                    if (cJSON_IsArray(selector_outbounds)) {
                        cJSON_AddItemToArray(selector_outbounds, cJSON_CreateString(newTagMb));
                    }
                    break; 
                }
            }
        }
    } else {
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
            cJSON_AddItemToArray(outbounds, nodeToPin);
            cJSON* last = cJSON_DetachItemFromArray(outbounds, cJSON_GetArraySize(outbounds)-1);
            cJSON_InsertItemInArray(outbounds, 0, last);
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
        if (is_duplicate && !is_current_node) {
            removed_count++;
            free(fingerprint);
            continue;
        }
        cJSON_AddItemToArray(new_outbounds, cJSON_Duplicate(node, 1));
        if (!is_duplicate) {
            seen_fingerprints[seen_count++] = fingerprint;
        } else {
            free(fingerprint);
        }
    }
    for (int i = 0; i < seen_count; i++) {
        free(seen_fingerprints[i]);
    }
    free(seen_fingerprints);
    free(currentNodeMb);
    cJSON_ReplaceItemInObject(root, "outbounds", new_outbounds);
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

static int compare_nodes_by_name(const void* a, const void* b) {
    const cJSON* nodeA = *(const cJSON**)a;
    const cJSON* nodeB = *(const cJSON**)b;
    cJSON* tagA_item = cJSON_GetObjectItem(nodeA, "tag");
    cJSON* tagB_item = cJSON_GetObjectItem(nodeB, "tag");
    const char* tagA = (cJSON_IsString(tagA_item) && tagA_item->valuestring) ? tagA_item->valuestring : "";
    const char* tagB = (cJSON_IsString(tagB_item) && tagB_item->valuestring) ? tagB_item->valuestring : "";
    return strcmp(tagA, tagB);
}

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
        return TRUE;
    }
    cJSON** nodes = (cJSON**)malloc(count * sizeof(cJSON*));
    if (!nodes) {
        cJSON_Delete(root);
        return FALSE;
    }
    for (int i = 0; i < count; i++) {
        nodes[i] = cJSON_DetachItemFromArray(outbounds, 0);
    }
    qsort(nodes, count, sizeof(cJSON*), compare_nodes_by_name);
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(outbounds, nodes[i]);
    }
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
        return 0;
    }
    char** seenTags = (char**)calloc(count, sizeof(char*));
    if (!seenTags) {
        cJSON_Delete(root);
        return -1;
    }
    int seenCount = 0;
    int renamedCount = 0;
    BOOL hasChanges = FALSE;
    char* currentActiveTagMb = NULL;
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, NULL, 0, NULL, NULL);
    currentActiveTagMb = (char*)malloc(mbLen);
    if (currentActiveTagMb) {
         WideCharToMultiByte(CP_UTF8, 0, currentNode, -1, currentActiveTagMb, mbLen, NULL, NULL);
    }
    cJSON* node = NULL;
    cJSON_ArrayForEach(node, outbounds) {
        cJSON* tagItem = cJSON_GetObjectItem(node, "tag");
        if (!cJSON_IsString(tagItem) || !tagItem->valuestring || strlen(tagItem->valuestring) == 0) {
            continue;
        }
        char* currentTag = tagItem->valuestring;
        BOOL isDuplicate = FALSE;
        for (int i = 0; i < seenCount; i++) {
            if (strcmp(seenTags[i], currentTag) == 0) {
                isDuplicate = TRUE;
                break;
            }
        }
        if (isDuplicate) {
            int suffix = 1;
            char newTagBuffer[512];
            BOOL uniqueTagFound = FALSE;
            while (!uniqueTagFound) {
                snprintf(newTagBuffer, sizeof(newTagBuffer), "%s (%d)", currentTag, suffix);
                BOOL newTagConflict = FALSE;
                for (int i = 0; i < seenCount; i++) {
                    if (strcmp(seenTags[i], newTagBuffer) == 0) {
                        newTagConflict = TRUE;
                        break;
                    }
                }
                if (!newTagConflict) {
                    uniqueTagFound = TRUE;
                    BOOL isCurrentActiveNode = (currentActiveTagMb && strcmp(currentTag, currentActiveTagMb) == 0);
                    cJSON_SetValuestring(tagItem, newTagBuffer);
                    if (isCurrentActiveNode) {
                        cJSON* route = cJSON_GetObjectItem(root, "route");
                        if (route) {
                            cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
                            if (final_outbound && strcmp(final_outbound->valuestring, currentActiveTagMb) == 0) {
                                cJSON_SetValuestring(final_outbound, newTagBuffer);
                            }
                        }
                    }
                    seenTags[seenCount] = strdup(newTagBuffer);
                    if (seenTags[seenCount]) {
                        seenCount++;
                    }
                    renamedCount++;
                    hasChanges = TRUE;
                } else {
                    suffix++;
                }
            }
        } else {
            seenTags[seenCount] = strdup(currentTag);
            if (seenTags[seenCount]) {
                seenCount++;
            }
        }
    }
    for (int i = 0; i < seenCount; i++) {
        free(seenTags[i]);
    }
    free(seenTags);
    if(currentActiveTagMb) free(currentActiveTagMb);
    if (hasChanges) {
        char* newContent = cJSON_PrintBuffered(root, 1, 1);
        if (newContent) {
            FILE* out = NULL;
            if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
                fwrite(newContent, 1, strlen(newContent), out);
                fclose(out);
            } else {
                renamedCount = -1;
            }
            free(newContent);
        } else {
            renamedCount = -1;
        }
    }
    cJSON_Delete(root);
    return renamedCount;
}

LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    const int MAX_LOG_LENGTH = 200000;
    const int TRIM_LOG_LENGTH = 100000;
    switch (msg) {
        case WM_CREATE: {
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                    0, 0, 0, 0,
                                    hWnd, (HMENU)ID_LOGVIEWER_EDIT,
                                    GetModuleHandle(NULL), NULL);
            if (hEdit == NULL) {
                ShowError(L"创建失败", L"无法创建日志显示框。");
                return -1;
            }
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hLogFont, TRUE);
            break;
        }
        case WM_LOG_UPDATE: {
            wchar_t* pLogChunk = (wchar_t*)lParam;
            if (pLogChunk) {
                int textLen = GetWindowTextLengthW(hEdit);
                if (textLen > MAX_LOG_LENGTH) {
                    SendMessageW(hEdit, EM_SETSEL, 0, TRIM_LOG_LENGTH);
                    SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)L"[... 日志已裁剪 ...]\r\n");
                }
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
                SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)pLogChunk);
                free(pLogChunk);
            }
            break;
        }
        case WM_SIZE: {
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            MoveWindow(hEdit, 0, 0, rcClient.right, rcClient.bottom, TRUE);
            break;
        }
        case WM_CLOSE: {
            ShowWindow(hWnd, SW_HIDE);
            break;
        }
        case WM_DESTROY: {
            hLogViewerWnd = NULL;
            break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenLogViewerWindow() {
    if (hLogViewerWnd != NULL) {
        ShowWindow(hLogViewerWnd, SW_SHOW);
        SetForegroundWindow(hLogViewerWnd);
        return;
    }
    const wchar_t* LOGVIEWER_CLASS_NAME = L"SingboxLogViewerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LogViewerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = LOGVIEWER_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
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
        hwnd,
        NULL, wc.hInstance, NULL
    );
    if (hLogViewerWnd) {
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

DWORD WINAPI InitThread(LPVOID lpParam) {
    HWND hWndMain = (HWND)lpParam;
    const wchar_t* configPath = L"config.json";
    wchar_t tempConfigPath[MAX_PATH] = {0};
    BOOL isRemoteMode = (wcslen(g_configUrl) > 0);

    #define THREAD_CLEANUP_AND_EXIT(success) \
        do { \
            if (tempConfigPath[0] != L'\0') DeleteFileW(tempConfigPath); \
            PostMessageW(hWndMain, WM_INIT_COMPLETE, (WPARAM)(success), (LPARAM)0); \
            return (success) ? 0 : 1; \
        } while (0)

    if (isRemoteMode) {
        wchar_t tempDir[MAX_PATH];
        DWORD tempPathLen = GetTempPathW(MAX_PATH, tempDir);
        if (tempPathLen == 0 || tempPathLen > MAX_PATH) {
            ShowError(L"启动失败", L"无法获取系统临时目录路径。");
            THREAD_CLEANUP_AND_EXIT(FALSE);
        }
        if (GetTempFileNameW(tempDir, L"sbx", 0, tempConfigPath) == 0) {
            ShowError(L"启动失败", L"无法在临时目录中创建临时文件。");
            tempConfigPath[0] = L'\0';
            THREAD_CLEANUP_AND_EXIT(FALSE);
        }
        if (!DownloadConfig(hWndMain, g_configUrl, tempConfigPath)) {
            DWORD fileAttrCache = GetFileAttributesW(configPath);
            if (fileAttrCache == INVALID_FILE_ATTRIBUTES || (fileAttrCache & FILE_ATTRIBUTE_DIRECTORY)) {
                 CreateDefaultConfig();
            }
            if (tempConfigPath[0] != L'\0') {
                DeleteFileW(tempConfigPath);
                tempConfigPath[0] = L'\0';
            }
        } else {
             DWORD fileAttr = GetFileAttributesW(configPath);
             BOOL configExists = (fileAttr != INVALID_FILE_ATTRIBUTES && !(fileAttr & FILE_ATTRIBUTE_DIRECTORY));
             if (configExists) {
                 long oldSize = 0, newSize = 0;
                 char* oldBuf = NULL, *newBuf = NULL;
                 ReadFileToBuffer(configPath, &oldBuf, &oldSize); 
                 if (oldBuf) free(oldBuf);
                 ReadFileToBuffer(tempConfigPath, &newBuf, &newSize);
                 if (newBuf) free(newBuf);
                 if (newSize > 0 && abs(newSize - oldSize) > 100) {
                     if (!MoveFileCrossVolumeW(tempConfigPath, configPath)) {
                         ShowError(L"配置更新失败", L"无法覆盖旧的 config.json。");
                         DeleteFileW(tempConfigPath);
                     }
                 } else {
                     DeleteFileW(tempConfigPath);
                 }
             } else {
                 if (!MoveFileCrossVolumeW(tempConfigPath, configPath)) {
                      ShowError(L"启动失败", L"无法将下载的配置 (tmp) 重命名为 config.json。");
                      DeleteFileW(tempConfigPath);
                      THREAD_CLEANUP_AND_EXIT(FALSE);
                 }
             }
             tempConfigPath[0] = L'\0';
        }
    } else {
        DWORD fileAttr = GetFileAttributesW(configPath);
        if (fileAttr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            CreateDefaultConfig();
        }
    }
    if (!ParseTags()) {
        MessageBoxW(NULL, L"无法读取或解析 config.json 文件。\n请检查文件是否存在且格式正确。", L"JSON 解析失败", MB_OK | MB_ICONERROR);
        THREAD_CLEANUP_AND_EXIT(FALSE);
    }
    int renamedCount = FixDuplicateTags();
    if (renamedCount == -1) {
        MessageBoxW(NULL, L"尝试自动修复重复标签时发生错误。\n请检查 config.json 文件权限。", L"修复错误", MB_OK | MB_ICONWARNING);
    } else if (renamedCount > 0) {
        if (!ParseTags()) {
            MessageBoxW(NULL, L"自动修复后无法重新加载 config.json。", L"错误", MB_OK | MB_ICONERROR);
            THREAD_CLEANUP_AND_EXIT(FALSE);
        }
    }
    #undef THREAD_CLEANUP_AND_EXIT
    g_isExiting = FALSE;
    StartSingBox();
    PostMessageW(hWndMain, WM_INIT_COMPLETE, (WPARAM)TRUE, (LPARAM)0);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    wchar_t mutexName[128];
    wchar_t guidString[40];

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"宋体");
    hLogFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hLogFont == NULL) {
        hLogFont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    }

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
        if (hLogFont) DeleteObject(hLogFont);
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

    LoadSettings();

    const wchar_t* CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont);
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
    wcsncpy(nid.szTip, L"程序正在启动...", ARRAYSIZE(nid.szTip) - 1);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = L'\0';

    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    HANDLE hInitThread = CreateThread(NULL, 0, InitThread, (LPVOID)hwnd, 0, NULL);
    if (hInitThread) {
        CloseHandle(hInitThread); 
    } else {
        ShowError(L"致命错误", L"无法创建启动线程。");
        if (g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont);
        DestroyWindow(hwnd);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (hLogViewerWnd == NULL || !IsDialogMessageW(hLogViewerWnd, &msg)) {
             TranslateMessage(&msg);
             DispatchMessage(&msg);
        }
    }
    
    if (!g_isExiting) {
         g_isExiting = TRUE;
         StopSingBox(); 
    }
    
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    CleanupDynamicNodes();
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    if (hLogFont) DeleteObject(hLogFont);
    if (g_hFont) DeleteObject(g_hFont);
    return (int)msg.wParam;
}