/**
 * Chrome Developer Launcher
 *
 * A Windows system tray application that:
 * - Launches Chrome with remote debugging enabled
 * - Sets up port forwarding for all network interfaces
 * - Monitors Chrome DevTools API status
 * - Provides configuration via registry-backed settings dialog
 *
 * Compile with MinGW: x86_64-w64-mingw32-gcc
 */

#define _WIN32_WINNT 0x0600
#define WINVER 0x0600

// Winsock must be included before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <iphlpapi.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ============================================================================
// Constants and Definitions
// ============================================================================

#define APP_NAME L"Chrome Developer Launcher"
#define MUTEX_NAME L"ChromeDevLauncher_SingleInstance_Mutex_A1B2C3D4"

// Registry
#define REG_COMPANY L"JPIT"
#define REG_APPNAME L"ChromeDevLauncher"
#define REG_KEY_PATH L"SOFTWARE\\JPIT\\ChromeDevLauncher"
#define REG_VALUE_CHROME_PATH L"ChromePath"
#define REG_VALUE_DEBUG_PORT L"DebugPort"
#define REG_VALUE_CONNECT_ADDRESS L"ConnectAddress"
#define REG_VALUE_STATUS_INTERVAL L"StatusCheckInterval"
#define REG_VALUE_CONFIGURED L"Configured"

// Tray icon
#define IDI_TRAYICON 101
#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1

// Menu IDs
#define ID_TRAY_MENU_STATUS 1
#define ID_TRAY_MENU_CONFIGURE 2
#define ID_TRAY_MENU_EXIT 3

// Dialog control IDs
#define IDC_EDIT_CHROME_PATH 1001
#define IDC_BTN_BROWSE 1002
#define IDC_EDIT_DEBUG_PORT 1003
#define IDC_EDIT_CONNECT_ADDR 1004
#define IDC_EDIT_STATUS_INTERVAL 1005
#define IDC_STATIC_CHROME_PATH 1006
#define IDC_STATIC_DEBUG_PORT 1007
#define IDC_STATIC_CONNECT_ADDR 1008
#define IDC_STATIC_STATUS_INTERVAL 1009

// Timers
#define ID_TIMER_STATUS_CHECK 1
#define ID_TIMER_CHROME_EXIT 2
#define CHROME_EXIT_CHECK_INTERVAL 1000

// Limits
#define MAX_INTERFACES 32
#define MAX_STATUS_TEXT 512

// ============================================================================
// Data Structures
// ============================================================================

typedef struct {
    wchar_t chromePath[MAX_PATH];
    int debugPort;
    wchar_t connectAddress[64];
    int statusCheckInterval;  // seconds
} Configuration;

typedef struct {
    char listenIP[16];
    int listenPort;
    BOOL active;
} PortForwardEntry;

typedef struct {
    BOOL chromeApiResponding;
    BOOL portForwardsActive;
    int activeForwardCount;
    char chromeVersion[64];                // Chrome version string
    wchar_t statusLine1[MAX_STATUS_TEXT];  // Main status
    wchar_t statusLine2[MAX_STATUS_TEXT];  // API status
    wchar_t statusLine3[MAX_STATUS_TEXT];  // Ports status
} StatusInfo;

// ============================================================================
// Global Variables
// ============================================================================

static HINSTANCE g_hInstance = NULL;
static HWND g_hwnd = NULL;
static HANDLE g_hMutex = NULL;
static NOTIFYICONDATAW g_nid = {0};

// Configuration
static Configuration g_config = {0};

// Chrome process
static HANDLE g_hJob = NULL;
static HANDLE g_hChromeProcess = NULL;
static DWORD g_dwChromePID = 0;

// Port forwards
static PortForwardEntry g_portForwards[MAX_INTERFACES] = {0};
static int g_portForwardCount = 0;

// Temp directory
static wchar_t g_szTempDir[MAX_PATH] = {0};

// Status
static StatusInfo g_status = {0};
static BOOL g_chromeRunning = FALSE;

// ============================================================================
// Forward Declarations
// ============================================================================

// Core
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static BOOL EnforceSingleInstance(void);
static BOOL IsRunningAsAdmin(void);
static void SelfElevate(void);

// Configuration
static BOOL LoadConfigFromRegistry(Configuration* config);
static BOOL SaveConfigToRegistry(const Configuration* config);
static BOOL IsFirstLaunch(void);
static void MarkAsConfigured(void);
static void SetDefaultConfig(Configuration* config);
static INT_PTR CALLBACK ConfigDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
static BOOL ShowConfigDialog(HWND hwndParent);

// Tray icon
static void CreateTrayIcon(HWND hwnd);
static void RemoveTrayIcon(void);
static void UpdateTrayTooltip(void);
static void ShowContextMenu(HWND hwnd);

// Network
static int EnumerateNonLoopbackInterfaces(PortForwardEntry* entries, int maxCount);
static BOOL AddPortForward(const char* listenIP, int listenPort, const char* connectIP, int connectPort);
static BOOL RemovePortForward(const char* listenIP, int listenPort);
static void SetupPortForwards(void);
static void CleanupAllPortForwards(void);

// Chrome
static BOOL CreateTempDirectory(void);
static void RemoveTempDirectory(void);
static BOOL LaunchChrome(void);
static void TerminateChrome(void);
static void RestartChrome(void);

// Status
static BOOL CheckChromeApiStatus(void);
static int CountActivePortForwards(void);
static void UpdateStatus(void);

// Cleanup
static void RegisterCleanupHandlers(void);
static void PerformCleanup(void);
static BOOL WINAPI ConsoleHandler(DWORD signal);
static LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* exInfo);

// ============================================================================
// Single Instance
// ============================================================================

static BOOL EnforceSingleInstance(void) {
    g_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL,
            L"Chrome Developer Launcher is already running.\n\n"
            L"Check your system tray for the application icon.",
            L"Already Running", MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }
    return TRUE;
}

// ============================================================================
// Admin Check & Self-Elevation
// ============================================================================

static BOOL IsRunningAsAdmin(void) {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;

    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin;
}

static void SelfElevate(void) {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
        SHELLEXECUTEINFOW sei = {0};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.hwnd = NULL;
        sei.nShow = SW_NORMAL;

        if (!ShellExecuteExW(&sei)) {
            DWORD err = GetLastError();
            if (err != ERROR_CANCELLED) {
                MessageBoxW(NULL, L"Failed to elevate to administrator.",
                           L"Error", MB_OK | MB_ICONERROR);
            }
        }
    }
}

// ============================================================================
// Configuration - Registry
// ============================================================================

static void SetDefaultConfig(Configuration* config) {
    config->chromePath[0] = L'\0';  // Empty - must be configured
    config->debugPort = 9222;
    wcscpy_s(config->connectAddress, 64, L"127.0.0.1");
    config->statusCheckInterval = 60;
}

static BOOL LoadConfigFromRegistry(Configuration* config) {
    SetDefaultConfig(config);

    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return FALSE;
    }

    DWORD dataSize;
    DWORD dataType;

    // Chrome Path
    dataSize = sizeof(config->chromePath);
    RegQueryValueExW(hKey, REG_VALUE_CHROME_PATH, NULL, &dataType,
                     (LPBYTE)config->chromePath, &dataSize);

    // Debug Port
    dataSize = sizeof(config->debugPort);
    RegQueryValueExW(hKey, REG_VALUE_DEBUG_PORT, NULL, &dataType,
                     (LPBYTE)&config->debugPort, &dataSize);

    // Connect Address
    dataSize = sizeof(config->connectAddress);
    RegQueryValueExW(hKey, REG_VALUE_CONNECT_ADDRESS, NULL, &dataType,
                     (LPBYTE)config->connectAddress, &dataSize);

    // Status Check Interval
    dataSize = sizeof(config->statusCheckInterval);
    RegQueryValueExW(hKey, REG_VALUE_STATUS_INTERVAL, NULL, &dataType,
                     (LPBYTE)&config->statusCheckInterval, &dataSize);

    RegCloseKey(hKey);
    return TRUE;
}

static BOOL SaveConfigToRegistry(const Configuration* config) {
    HKEY hKey;
    DWORD disposition;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL,
                                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                                   &hKey, &disposition);
    if (result != ERROR_SUCCESS) {
        return FALSE;
    }

    // Chrome Path
    RegSetValueExW(hKey, REG_VALUE_CHROME_PATH, 0, REG_SZ,
                   (const BYTE*)config->chromePath,
                   (DWORD)((wcslen(config->chromePath) + 1) * sizeof(wchar_t)));

    // Debug Port
    RegSetValueExW(hKey, REG_VALUE_DEBUG_PORT, 0, REG_DWORD,
                   (const BYTE*)&config->debugPort, sizeof(config->debugPort));

    // Connect Address
    RegSetValueExW(hKey, REG_VALUE_CONNECT_ADDRESS, 0, REG_SZ,
                   (const BYTE*)config->connectAddress,
                   (DWORD)((wcslen(config->connectAddress) + 1) * sizeof(wchar_t)));

    // Status Check Interval
    RegSetValueExW(hKey, REG_VALUE_STATUS_INTERVAL, 0, REG_DWORD,
                   (const BYTE*)&config->statusCheckInterval,
                   sizeof(config->statusCheckInterval));

    RegCloseKey(hKey);
    return TRUE;
}

static BOOL IsFirstLaunch(void) {
    HKEY hKey;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return TRUE;
    }

    DWORD configured = 0;
    DWORD dataSize = sizeof(configured);
    result = RegQueryValueExW(hKey, REG_VALUE_CONFIGURED, NULL, NULL,
                              (LPBYTE)&configured, &dataSize);
    RegCloseKey(hKey);

    return (result != ERROR_SUCCESS || configured == 0);
}

static void MarkAsConfigured(void) {
    HKEY hKey;
    DWORD disposition;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL,
                                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                                   &hKey, &disposition);
    if (result == ERROR_SUCCESS) {
        DWORD configured = 1;
        RegSetValueExW(hKey, REG_VALUE_CONFIGURED, 0, REG_DWORD,
                       (const BYTE*)&configured, sizeof(configured));
        RegCloseKey(hKey);
    }
}

// ============================================================================
// Configuration Dialog
// ============================================================================

static INT_PTR CALLBACK ConfigDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static Configuration* pTempConfig = NULL;

    switch (message) {
        case WM_INITDIALOG: {
            pTempConfig = (Configuration*)lParam;
            if (!pTempConfig) return FALSE;

            // Set edit control text
            SetDlgItemTextW(hDlg, IDC_EDIT_CHROME_PATH, pTempConfig->chromePath);
            SetDlgItemInt(hDlg, IDC_EDIT_DEBUG_PORT, pTempConfig->debugPort, FALSE);
            SetDlgItemTextW(hDlg, IDC_EDIT_CONNECT_ADDR, pTempConfig->connectAddress);
            SetDlgItemInt(hDlg, IDC_EDIT_STATUS_INTERVAL, pTempConfig->statusCheckInterval, FALSE);

            // Center dialog on screen
            RECT rcDlg, rcScreen;
            GetWindowRect(hDlg, &rcDlg);
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcScreen, 0);
            int x = rcScreen.left + ((rcScreen.right - rcScreen.left) - (rcDlg.right - rcDlg.left)) / 2;
            int y = rcScreen.top + ((rcScreen.bottom - rcScreen.top) - (rcDlg.bottom - rcDlg.top)) / 2;
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BTN_BROWSE: {
                    OPENFILENAMEW ofn = {0};
                    wchar_t szFile[MAX_PATH] = {0};
                    GetDlgItemTextW(hDlg, IDC_EDIT_CHROME_PATH, szFile, MAX_PATH);

                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hDlg;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrTitle = L"Select Chrome Executable";
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hDlg, IDC_EDIT_CHROME_PATH, szFile);
                    }
                    return TRUE;
                }

                case IDOK: {
                    if (!pTempConfig) {
                        EndDialog(hDlg, IDCANCEL);
                        return TRUE;
                    }

                    // Get values from edit controls
                    GetDlgItemTextW(hDlg, IDC_EDIT_CHROME_PATH, pTempConfig->chromePath, MAX_PATH);
                    pTempConfig->debugPort = GetDlgItemInt(hDlg, IDC_EDIT_DEBUG_PORT, NULL, FALSE);
                    GetDlgItemTextW(hDlg, IDC_EDIT_CONNECT_ADDR, pTempConfig->connectAddress, 64);
                    pTempConfig->statusCheckInterval = GetDlgItemInt(hDlg, IDC_EDIT_STATUS_INTERVAL, NULL, FALSE);

                    // Validate port
                    if (pTempConfig->debugPort < 1 || pTempConfig->debugPort > 65535) {
                        MessageBoxW(hDlg, L"Debug port must be between 1 and 65535.",
                                   L"Validation Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_EDIT_DEBUG_PORT));
                        return TRUE;
                    }

                    // Validate interval
                    if (pTempConfig->statusCheckInterval < 5) {
                        MessageBoxW(hDlg, L"Status check interval must be at least 5 seconds.",
                                   L"Validation Error", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hDlg, IDC_EDIT_STATUS_INTERVAL));
                        return TRUE;
                    }

                    // Connect address default
                    if (pTempConfig->connectAddress[0] == L'\0') {
                        wcscpy_s(pTempConfig->connectAddress, 64, L"127.0.0.1");
                    }

                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_CLOSE:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// Helper to add a control to dialog template (from SystrayLauncher)
static BYTE* AddDialogControl2(BYTE* ptr, WORD ctrlId, WORD classAtom, DWORD style,
                               short posX, short posY, short width, short height,
                               const wchar_t* text) {
    // Align to DWORD
    ptr = (BYTE*)(((ULONG_PTR)ptr + 3) & ~3);

    DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)ptr;
    item->style = style;
    item->dwExtendedStyle = 0;
    item->x = posX;
    item->y = posY;
    item->cx = width;
    item->cy = height;
    item->id = ctrlId;
    ptr += sizeof(DLGITEMTEMPLATE);

    // Class (atom)
    *(WORD*)ptr = 0xFFFF;
    ptr += sizeof(WORD);
    *(WORD*)ptr = classAtom;
    ptr += sizeof(WORD);

    // Text
    size_t textLen = wcslen(text) + 1;
    memcpy(ptr, text, textLen * sizeof(wchar_t));
    ptr += textLen * sizeof(wchar_t);

    // Creation data (none)
    *(WORD*)ptr = 0;
    ptr += sizeof(WORD);

    return ptr;
}

static BOOL ShowConfigDialog(HWND hwndParent) {
    // Create a copy of current config to edit
    Configuration tempConfig;
    memcpy(&tempConfig, &g_config, sizeof(Configuration));

    // Dialog dimensions (in dialog units) - matching SystrayLauncher
    const short DLG_WIDTH = 320;
    const short MARGIN_X = 8;
    const short MARGIN_Y = 8;
    const short LABEL_H = 10;
    const short LABEL_GAP = 2;
    const short EDIT_H = 14;
    const short SPACING = 6;
    const short BTN_W = 50;
    const short BTN_H = 14;
    const short BROWSE_W = 20;

    // Calculate dialog height: 4 fields + buttons
    const short DLG_HEIGHT = MARGIN_Y
        + (LABEL_H + LABEL_GAP + EDIT_H + SPACING) * 4
        + BTN_H + MARGIN_Y;

    short editW = DLG_WIDTH - (2 * MARGIN_X);
    short pathEditW = editW - BROWSE_W - 4;
    short yPos = MARGIN_Y;

    // Allocate buffer for dialog template
    size_t templateSize = 4096;
    BYTE* templateBuffer = (BYTE*)calloc(1, templateSize);
    if (!templateBuffer) return FALSE;

    BYTE* ptr = templateBuffer;

    // DLGTEMPLATE - must be DWORD aligned (buffer starts aligned from calloc)
    DLGTEMPLATE* dlgTemplate = (DLGTEMPLATE*)ptr;
    dlgTemplate->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT;
    dlgTemplate->dwExtendedStyle = 0;
    dlgTemplate->cdit = 11;  // 4 labels + 4 edits + browse + OK + Cancel
    dlgTemplate->x = 0;
    dlgTemplate->y = 0;
    dlgTemplate->cx = DLG_WIDTH;
    dlgTemplate->cy = DLG_HEIGHT;
    ptr += sizeof(DLGTEMPLATE);

    // Menu (none) - WORD array
    *(WORD*)ptr = 0;
    ptr += sizeof(WORD);

    // Class (default) - WORD array
    *(WORD*)ptr = 0;
    ptr += sizeof(WORD);

    // Title - null-terminated Unicode string
    const wchar_t* title = L"Configuration";
    size_t titleLen = wcslen(title) + 1;
    memcpy(ptr, title, titleLen * sizeof(wchar_t));
    ptr += titleLen * sizeof(wchar_t);

    // Font (required for DS_SETFONT)
    *(WORD*)ptr = 8;  // Point size
    ptr += sizeof(WORD);

    // Font name - null-terminated Unicode string
    const wchar_t* fontName = L"Segoe UI";
    size_t fontLen = wcslen(fontName) + 1;
    memcpy(ptr, fontName, fontLen * sizeof(wchar_t));
    ptr += fontLen * sizeof(wchar_t);

    // Now add controls - each must be DWORD aligned

    // Chrome Path label
    ptr = AddDialogControl2(ptr, IDC_STATIC_CHROME_PATH, 0x0082,
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            MARGIN_X, yPos, editW, LABEL_H,
                            L"Chrome Executable Path:");
    yPos += LABEL_H + LABEL_GAP;

    // Chrome Path edit
    ptr = AddDialogControl2(ptr, IDC_EDIT_CHROME_PATH, 0x0081,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                            MARGIN_X, yPos, pathEditW, EDIT_H, L"");

    // Browse button
    ptr = AddDialogControl2(ptr, IDC_BTN_BROWSE, 0x0080,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            MARGIN_X + pathEditW + 2, yPos, BROWSE_W, EDIT_H, L"...");
    yPos += EDIT_H + SPACING;

    // Debug Port label
    ptr = AddDialogControl2(ptr, IDC_STATIC_DEBUG_PORT, 0x0082,
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            MARGIN_X, yPos, editW, LABEL_H,
                            L"Debug Port:");
    yPos += LABEL_H + LABEL_GAP;

    // Debug Port edit
    ptr = AddDialogControl2(ptr, IDC_EDIT_DEBUG_PORT, 0x0081,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER,
                            MARGIN_X, yPos, editW, EDIT_H, L"");
    yPos += EDIT_H + SPACING;

    // Chrome IP Address label
    ptr = AddDialogControl2(ptr, IDC_STATIC_CONNECT_ADDR, 0x0082,
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            MARGIN_X, yPos, editW, LABEL_H,
                            L"Chrome IP Address:");
    yPos += LABEL_H + LABEL_GAP;

    // Connect Address edit
    ptr = AddDialogControl2(ptr, IDC_EDIT_CONNECT_ADDR, 0x0081,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                            MARGIN_X, yPos, editW, EDIT_H, L"");
    yPos += EDIT_H + SPACING;

    // Status Interval label
    ptr = AddDialogControl2(ptr, IDC_STATIC_STATUS_INTERVAL, 0x0082,
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            MARGIN_X, yPos, editW, LABEL_H,
                            L"Status Check Interval (seconds):");
    yPos += LABEL_H + LABEL_GAP;

    // Status Interval edit
    ptr = AddDialogControl2(ptr, IDC_EDIT_STATUS_INTERVAL, 0x0081,
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER,
                            MARGIN_X, yPos, editW, EDIT_H, L"");
    yPos += EDIT_H + SPACING;

    // Buttons
    short btnY = yPos;
    short okX = DLG_WIDTH - MARGIN_X - BTN_W - 4 - BTN_W;
    ptr = AddDialogControl2(ptr, IDOK, 0x0080,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            okX, btnY, BTN_W, BTN_H, L"OK");

    short cancelX = DLG_WIDTH - MARGIN_X - BTN_W;
    ptr = AddDialogControl2(ptr, IDCANCEL, 0x0080,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            cancelX, btnY, BTN_W, BTN_H, L"Cancel");

    INT_PTR result = DialogBoxIndirectParamW(g_hInstance, (DLGTEMPLATE*)templateBuffer,
                                              hwndParent, ConfigDialogProc, (LPARAM)&tempConfig);

    free(templateBuffer);

    if (result == IDOK) {
        // Check if Chrome-impacting settings changed
        BOOL needsRestart = FALSE;
        if (wcscmp(g_config.chromePath, tempConfig.chromePath) != 0 ||
            g_config.debugPort != tempConfig.debugPort ||
            wcscmp(g_config.connectAddress, tempConfig.connectAddress) != 0) {
            needsRestart = TRUE;
        }

        // Copy temp config to global config
        memcpy(&g_config, &tempConfig, sizeof(Configuration));

        // Save to registry
        SaveConfigToRegistry(&g_config);
        MarkAsConfigured();

        // Restart Chrome if needed
        if (needsRestart && g_chromeRunning) {
            RestartChrome();
        } else if (!g_chromeRunning && g_config.chromePath[0] != L'\0') {
            SetupPortForwards();
            LaunchChrome();
        }

        // Update status check timer
        if (g_hwnd) {
            KillTimer(g_hwnd, ID_TIMER_STATUS_CHECK);
            SetTimer(g_hwnd, ID_TIMER_STATUS_CHECK, g_config.statusCheckInterval * 1000, NULL);
        }

        UpdateStatus();
        UpdateTrayTooltip();

        return TRUE;
    }

    return FALSE;
}

// ============================================================================
// Tray Icon
// ============================================================================

static void CreateTrayIcon(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    // Load icon from resources
    HDC hdcScreen = GetDC(NULL);
    int dpiX = GetDeviceCaps(hdcScreen, LOGPIXELSX);
    ReleaseDC(NULL, hdcScreen);

    int iconSize = (dpiX >= 120) ? 32 : 16;
    g_nid.hIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON),
                                     IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR);

    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    }

    wcscpy_s(g_nid.szTip, sizeof(g_nid.szTip)/sizeof(wchar_t), APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void) {
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = NULL;
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void UpdateTrayTooltip(void) {
    // Tooltip can have newlines, so combine all lines
    swprintf_s(g_nid.szTip, sizeof(g_nid.szTip)/sizeof(wchar_t),
               L"%ls\n%ls\n%ls", g_status.statusLine1, g_status.statusLine2, g_status.statusLine3);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();

    // Status lines (grayed) - each on separate menu item
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, g_status.statusLine1);
    if (g_status.statusLine2[0] != L'\0') {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, g_status.statusLine2);
    }
    if (g_status.statusLine3[0] != L'\0') {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, g_status.statusLine3);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_CONFIGURE, L"Configure");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_MENU_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ============================================================================
// Network Interface Enumeration
// ============================================================================

static int EnumerateNonLoopbackInterfaces(PortForwardEntry* entries, int maxCount) {
    int count = 0;

    ULONG bufferSize = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(bufferSize);
    if (!pAddresses) return 0;

    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                         NULL, pAddresses, &bufferSize);

    if (result == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(bufferSize);
        if (!pAddresses) return 0;
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                       NULL, pAddresses, &bufferSize);
    }

    if (result != NO_ERROR) {
        free(pAddresses);
        return 0;
    }

    PIP_ADAPTER_ADDRESSES pCurrent = pAddresses;
    while (pCurrent && count < maxCount) {
        // Skip loopback adapter
        if (pCurrent->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            pCurrent = pCurrent->Next;
            continue;
        }

        // Skip adapters that are not up
        if (pCurrent->OperStatus != IfOperStatusUp) {
            pCurrent = pCurrent->Next;
            continue;
        }

        // Get unicast addresses
        PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurrent->FirstUnicastAddress;
        while (pUnicast && count < maxCount) {
            if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in* addr = (struct sockaddr_in*)pUnicast->Address.lpSockaddr;
                char* ipStr = inet_ntoa(addr->sin_addr);

                // Skip 127.x.x.x addresses
                if (strncmp(ipStr, "127.", 4) != 0) {
                    strncpy_s(entries[count].listenIP, 16, ipStr, 15);
                    entries[count].listenPort = 0;  // Will be set when adding forward
                    entries[count].active = FALSE;
                    count++;
                }
            }
            pUnicast = pUnicast->Next;
        }

        pCurrent = pCurrent->Next;
    }

    free(pAddresses);
    return count;
}

// ============================================================================
// Port Forwarding
// ============================================================================

static BOOL AddPortForward(const char* listenIP, int listenPort, const char* connectIP, int connectPort) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "netsh interface portproxy add v4tov4 listenaddress=%s listenport=%d "
             "connectaddress=%s connectport=%d",
             listenIP, listenPort, connectIP, connectPort);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL success = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    if (success) {
        WaitForSingleObject(pi.hProcess, 5000);
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (exitCode == 0);
    }

    return FALSE;
}

static BOOL RemovePortForward(const char* listenIP, int listenPort) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "netsh interface portproxy delete v4tov4 listenaddress=%s listenport=%d",
             listenIP, listenPort);

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL success = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    if (success) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return TRUE;
    }

    return FALSE;
}

static void SetupPortForwards(void) {
    // First, clean up any existing forwards
    CleanupAllPortForwards();

    // Enumerate interfaces
    g_portForwardCount = EnumerateNonLoopbackInterfaces(g_portForwards, MAX_INTERFACES);

    // Convert connect address to narrow string
    char connectAddr[64];
    WideCharToMultiByte(CP_UTF8, 0, g_config.connectAddress, -1, connectAddr, 64, NULL, NULL);

    // Add port forward for each interface
    for (int i = 0; i < g_portForwardCount; i++) {
        g_portForwards[i].listenPort = g_config.debugPort;

        if (AddPortForward(g_portForwards[i].listenIP, g_portForwards[i].listenPort,
                           connectAddr, g_config.debugPort)) {
            g_portForwards[i].active = TRUE;
        } else {
            // Log failure but continue (graceful handling)
            g_portForwards[i].active = FALSE;
        }
    }
}

static void CleanupAllPortForwards(void) {
    for (int i = 0; i < g_portForwardCount; i++) {
        if (g_portForwards[i].active) {
            RemovePortForward(g_portForwards[i].listenIP, g_portForwards[i].listenPort);
            g_portForwards[i].active = FALSE;
        }
    }
}

static int CountActivePortForwards(void) {
    int count = 0;
    for (int i = 0; i < g_portForwardCount; i++) {
        if (g_portForwards[i].active) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// Temp Directory
// ============================================================================

static BOOL CreateTempDirectory(void) {
    wchar_t tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempPath)) {
        return FALSE;
    }

    // Generate unique directory name
    swprintf_s(g_szTempDir, MAX_PATH, L"%schrome_debug_%u", tempPath, GetCurrentProcessId());

    return CreateDirectoryW(g_szTempDir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void RemoveTempDirectory(void) {
    if (g_szTempDir[0] == L'\0') return;

    // Use SHFileOperation for recursive delete
    wchar_t dirPath[MAX_PATH + 2] = {0};  // Double null-terminated
    wcscpy_s(dirPath, MAX_PATH, g_szTempDir);

    SHFILEOPSTRUCTW fileOp = {0};
    fileOp.hwnd = NULL;
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = dirPath;
    fileOp.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    SHFileOperationW(&fileOp);
    g_szTempDir[0] = L'\0';
}

// ============================================================================
// Chrome Process Management
// ============================================================================

static BOOL LaunchChrome(void) {
    if (g_config.chromePath[0] == L'\0') {
        return FALSE;
    }

    // Create temp directory for user data
    if (!CreateTempDirectory()) {
        return FALSE;
    }

    // Create job object
    g_hJob = CreateJobObjectW(NULL, NULL);
    if (!g_hJob) {
        RemoveTempDirectory();
        return FALSE;
    }

    // Configure job to terminate all processes when closed
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {0};
    jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_hJob, JobObjectExtendedLimitInformation,
                             &jobInfo, sizeof(jobInfo));

    // Build command line
    wchar_t cmdLine[MAX_PATH * 2];
    swprintf_s(cmdLine, sizeof(cmdLine)/sizeof(wchar_t),
               L"\"%s\" --remote-debugging-port=%d --user-data-dir=\"%s\"",
               g_config.chromePath, g_config.debugPort, g_szTempDir);

    // Launch Chrome
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);

    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                                   CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                                   NULL, NULL, &si, &pi);

    if (!success) {
        CloseHandle(g_hJob);
        g_hJob = NULL;
        RemoveTempDirectory();
        return FALSE;
    }

    // Assign to job
    AssignProcessToJobObject(g_hJob, pi.hProcess);

    // Resume the process
    ResumeThread(pi.hThread);

    g_hChromeProcess = pi.hProcess;
    g_dwChromePID = pi.dwProcessId;
    g_chromeRunning = TRUE;

    CloseHandle(pi.hThread);

    return TRUE;
}

static void TerminateChrome(void) {
    if (g_hJob) {
        TerminateJobObject(g_hJob, 0);
        CloseHandle(g_hJob);
        g_hJob = NULL;
    }

    if (g_hChromeProcess) {
        CloseHandle(g_hChromeProcess);
        g_hChromeProcess = NULL;
    }

    g_dwChromePID = 0;
    g_chromeRunning = FALSE;

    CleanupAllPortForwards();
    RemoveTempDirectory();
}

static void RestartChrome(void) {
    TerminateChrome();
    Sleep(500);  // Brief pause
    SetupPortForwards();
    LaunchChrome();
}

// ============================================================================
// Status Checking
// ============================================================================

static BOOL CheckChromeApiStatus(void) {
    char url[128];
    char connectAddr[64];
    WideCharToMultiByte(CP_UTF8, 0, g_config.connectAddress, -1, connectAddr, 64, NULL, NULL);

    // Fetch /json/version to get Chrome version
    snprintf(url, sizeof(url), "http://%s:%d/json/version", connectAddr, g_config.debugPort);

    HINTERNET hInternet = InternetOpenA("ChromeDevLauncher",
                                         INTERNET_OPEN_TYPE_DIRECT,
                                         NULL, NULL, 0);
    if (!hInternet) return FALSE;

    HINTERNET hConnect = InternetOpenUrlA(hInternet, url, NULL, 0,
                                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
                                           0);

    BOOL success = FALSE;
    g_status.chromeVersion[0] = '\0';

    if (hConnect) {
        char buffer[1024];
        DWORD bytesRead;
        if (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead)) {
            buffer[bytesRead] = '\0';
            // Check for Browser field in JSON response
            char* browserField = strstr(buffer, "\"Browser\"");
            if (browserField) {
                success = TRUE;
                // Parse version: "Browser": "Chrome/141.0.7390.123"
                char* colonPos = strchr(browserField, ':');
                if (colonPos) {
                    char* startQuote = strchr(colonPos, '"');
                    if (startQuote) {
                        startQuote++; // Skip opening quote
                        char* endQuote = strchr(startQuote, '"');
                        if (endQuote) {
                            size_t len = endQuote - startQuote;
                            if (len < sizeof(g_status.chromeVersion)) {
                                strncpy(g_status.chromeVersion, startQuote, len);
                                g_status.chromeVersion[len] = '\0';
                            }
                        }
                    }
                }
            }
        }
        InternetCloseHandle(hConnect);
    }

    InternetCloseHandle(hInternet);
    return success;
}

static void UpdateStatus(void) {
    // Check Chrome API
    g_status.chromeApiResponding = CheckChromeApiStatus();

    // Check port forwards
    g_status.activeForwardCount = CountActivePortForwards();
    g_status.portForwardsActive = (g_status.activeForwardCount > 0);

    // Build status text lines
    g_status.statusLine2[0] = L'\0';
    g_status.statusLine3[0] = L'\0';

    // Build port list string for active ports
    wchar_t portList[128] = {0};
    if (g_status.portForwardsActive) {
        wchar_t* ptr = portList;
        size_t remaining = sizeof(portList) / sizeof(wchar_t);
        BOOL first = TRUE;
        for (int i = 0; i < g_portForwardCount && remaining > 10; i++) {
            if (g_portForwards[i].active) {
                if (!first) {
                    *ptr++ = L',';
                    remaining--;
                }
                int written = swprintf_s(ptr, remaining, L"%d", g_portForwards[i].listenPort);
                if (written > 0) {
                    ptr += written;
                    remaining -= written;
                }
                first = FALSE;
            }
        }
    }

    // Get version without "Chrome/" prefix
    const char* versionStr = g_status.chromeVersion;
    if (strncmp(versionStr, "Chrome/", 7) == 0) {
        versionStr += 7;  // Skip "Chrome/"
    }

    if (g_config.chromePath[0] == L'\0') {
        wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Not configured");
    } else if (!g_chromeRunning) {
        wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome not running");
    } else if (g_status.chromeApiResponding && g_status.portForwardsActive) {
        if (versionStr[0]) {
            wchar_t versionW[64];
            MultiByteToWideChar(CP_UTF8, 0, versionStr, -1, versionW, 64);
            swprintf_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: %ls", versionW);
        } else {
            wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: Connected");
        }
        wcscpy_s(g_status.statusLine2, MAX_STATUS_TEXT, L"API: Responding");
        swprintf_s(g_status.statusLine3, MAX_STATUS_TEXT, L"Ports: Active (%ls)", portList);
    } else if (g_status.chromeApiResponding) {
        if (versionStr[0]) {
            wchar_t versionW[64];
            MultiByteToWideChar(CP_UTF8, 0, versionStr, -1, versionW, 64);
            swprintf_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: %ls", versionW);
        } else {
            wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: Connected");
        }
        wcscpy_s(g_status.statusLine2, MAX_STATUS_TEXT, L"API: Responding");
        wcscpy_s(g_status.statusLine3, MAX_STATUS_TEXT, L"Ports: None active");
    } else if (g_status.portForwardsActive) {
        wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: Not responding");
        wcscpy_s(g_status.statusLine2, MAX_STATUS_TEXT, L"API: Not responding");
        swprintf_s(g_status.statusLine3, MAX_STATUS_TEXT, L"Ports: Active (%ls)", portList);
    } else {
        wcscpy_s(g_status.statusLine1, MAX_STATUS_TEXT, L"Chrome: Not responding");
        wcscpy_s(g_status.statusLine2, MAX_STATUS_TEXT, L"API: Not responding");
        wcscpy_s(g_status.statusLine3, MAX_STATUS_TEXT, L"Ports: None");
    }
}

// ============================================================================
// Cleanup Handlers
// ============================================================================

static BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_BREAK_EVENT || signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        PerformCleanup();
        return TRUE;
    }
    return FALSE;
}

static LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* exInfo) {
    (void)exInfo;
    PerformCleanup();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void AtExitHandler(void) {
    CleanupAllPortForwards();
}

static void RegisterCleanupHandlers(void) {
    atexit(AtExitHandler);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetUnhandledExceptionFilter(ExceptionHandler);
}

static void PerformCleanup(void) {
    // Remove tray icon
    RemoveTrayIcon();

    // Terminate Chrome and clean up port forwards
    TerminateChrome();

    // Release mutex
    if (g_hMutex) {
        ReleaseMutex(g_hMutex);
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
    }
}

// ============================================================================
// Window Procedure
// ============================================================================

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_STATUS_CHECK) {
                UpdateStatus();
                UpdateTrayTooltip();
            } else if (wParam == ID_TIMER_CHROME_EXIT) {
                // Check if Chrome is still running by querying Job Object
                // Don't check the main process - it exits quickly due to Chrome's architecture
                if (g_hJob && g_chromeRunning) {
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION jobInfo;
                    if (QueryInformationJobObject(g_hJob, JobObjectBasicAccountingInformation,
                                                   &jobInfo, sizeof(jobInfo), NULL)) {
                        if (jobInfo.ActiveProcesses == 0) {
                            // All processes in the job have exited
                            TerminateChrome();
                            UpdateStatus();
                            UpdateTrayTooltip();
                        }
                    }
                }
            }
            return 0;

        case WM_TRAYICON:
            switch (lParam) {
                case WM_LBUTTONDBLCLK:
                    ShowConfigDialog(hwnd);
                    break;
                case WM_RBUTTONUP:
                    ShowContextMenu(hwnd);
                    break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_MENU_CONFIGURE:
                    ShowConfigDialog(hwnd);
                    return 0;
                case ID_TRAY_MENU_EXIT:
                    PerformCleanup();
                    PostQuitMessage(0);
                    return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_STATUS_CHECK);
            KillTimer(hwnd, ID_TIMER_CHROME_EXIT);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// Entry Point
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hInstance = hInstance;

    // Admin check FIRST (before mutex, so elevated process can acquire it)
    BOOL isAdmin = IsRunningAsAdmin();

    if (!isAdmin) {
        SelfElevate();
        return 0;
    }

    // Single instance check (after elevation)
    if (!EnforceSingleInstance()) {
        return 0;
    }

    // Register cleanup handlers
    RegisterCleanupHandlers();

    // Load configuration
    if (!LoadConfigFromRegistry(&g_config)) {
        SetDefaultConfig(&g_config);
    }

    // First launch check - just mark as configured, user can configure via tray menu
    BOOL isFirstLaunch = IsFirstLaunch();

    if (isFirstLaunch) {
        // Set a default Chrome path for common locations
        if (g_config.chromePath[0] == L'\0') {
            const wchar_t* chromePaths[] = {
                L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
                L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
                NULL
            };
            for (int i = 0; chromePaths[i] != NULL; i++) {
                if (GetFileAttributesW(chromePaths[i]) != INVALID_FILE_ATTRIBUTES) {
                    wcscpy_s(g_config.chromePath, MAX_PATH, chromePaths[i]);
                    break;
                }
            }
        }
        SaveConfigToRegistry(&g_config);
        MarkAsConfigured();
    }

    // Register window class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ChromeDevLauncherClass";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));

    if (!RegisterClassExW(&wc)) {
        PerformCleanup();
        return 1;
    }

    // Create hidden window for message processing
    g_hwnd = CreateWindowExW(0, L"ChromeDevLauncherClass", APP_NAME,
                              0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, hInstance, NULL);
    if (!g_hwnd) {
        PerformCleanup();
        return 1;
    }

    // Create tray icon
    CreateTrayIcon(g_hwnd);

    // Setup port forwards and launch Chrome if configured
    if (g_config.chromePath[0] != L'\0') {
        SetupPortForwards();
        if (!LaunchChrome()) {
            MessageBoxW(NULL, L"Failed to launch Chrome.\n\n"
                             L"Please check your Chrome path in Configuration.",
                       L"Error", MB_OK | MB_ICONERROR);
        }
    }

    // Initial status update
    UpdateStatus();
    UpdateTrayTooltip();

    // Start timers
    SetTimer(g_hwnd, ID_TIMER_STATUS_CHECK, g_config.statusCheckInterval * 1000, NULL);
    SetTimer(g_hwnd, ID_TIMER_CHROME_EXIT, CHROME_EXIT_CHECK_INTERVAL, NULL);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Final cleanup
    PerformCleanup();

    return (int)msg.wParam;
}
