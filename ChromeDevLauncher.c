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

#ifndef OBJID_WINDOW
#define OBJID_WINDOW 0
#endif

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
#include <objbase.h>

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
#define ID_TRAY_MENU_EXIT 4

// Custom messages
#define WM_BRING_CHROME_TO_FRONT (WM_USER + 100)

// Resource IDs
#define IDR_HTML_UI      200
#define IDR_WEBVIEW2_DLL 201

// Timers
#define ID_TIMER_STATUS_CHECK 1
#define ID_TIMER_CHROME_EXIT 2
#define CHROME_EXIT_CHECK_INTERVAL 1000
#define ID_TIMER_WEBVIEW_SHOW_FALLBACK 1006
#define WEBVIEW_SHOW_FALLBACK_DELAY_MS 350

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
static BOOL g_chromeHidden = TRUE;  // Start hidden, restore on tray double-click
static HWINEVENTHOOK g_hWinEventHook = NULL;  // Hook for real-time window detection

// ============================================================================
// WebView2 COM interface definitions (minimal vtable approach)
// ============================================================================

// GUIDs
DEFINE_GUID(IID_ICoreWebView2Environment, 0xb96d755e,0x0319,0x4e92,0xa2,0x96,0x23,0x43,0x6f,0x46,0xa1,0xfc);
DEFINE_GUID(IID_ICoreWebView2Controller, 0x4d00c0d1,0x9583,0x4f38,0x8e,0x50,0xa9,0xa6,0xb3,0x44,0x78,0xcd);
DEFINE_GUID(IID_ICoreWebView2, 0x76eceacb,0x0462,0x4d94,0xac,0x83,0x42,0x3a,0x67,0x93,0x77,0x5e);
DEFINE_GUID(IID_ICoreWebView2Settings, 0xe562e4f0,0xd7fa,0x43ac,0x8d,0x71,0xc0,0x51,0x50,0x49,0x9f,0x00);

typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;

// Forward declarations of COM interfaces
typedef struct ICoreWebView2Environment ICoreWebView2Environment;
typedef struct ICoreWebView2Controller ICoreWebView2Controller;
typedef struct ICoreWebView2 ICoreWebView2;
typedef struct ICoreWebView2Settings ICoreWebView2Settings;
typedef struct ICoreWebView2WebMessageReceivedEventArgs ICoreWebView2WebMessageReceivedEventArgs;
typedef struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
typedef struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
typedef struct ICoreWebView2WebMessageReceivedEventHandler ICoreWebView2WebMessageReceivedEventHandler;

// ICoreWebView2Environment vtable
typedef struct ICoreWebView2EnvironmentVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Environment*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Environment*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Environment*);
    HRESULT (STDMETHODCALLTYPE *CreateCoreWebView2Controller)(ICoreWebView2Environment*, HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *CreateWebResourceResponse)(ICoreWebView2Environment*, void*, int, LPCWSTR, LPCWSTR, void**);
    HRESULT (STDMETHODCALLTYPE *get_BrowserVersionString)(ICoreWebView2Environment*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *add_NewBrowserVersionAvailable)(ICoreWebView2Environment*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NewBrowserVersionAvailable)(ICoreWebView2Environment*, EventRegistrationToken);
} ICoreWebView2EnvironmentVtbl;

struct ICoreWebView2Environment { const ICoreWebView2EnvironmentVtbl *lpVtbl; };

// ICoreWebView2Controller vtable
typedef struct ICoreWebView2ControllerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Controller*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Controller*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *get_IsVisible)(ICoreWebView2Controller*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsVisible)(ICoreWebView2Controller*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_Bounds)(ICoreWebView2Controller*, RECT*);
    HRESULT (STDMETHODCALLTYPE *put_Bounds)(ICoreWebView2Controller*, RECT);
    HRESULT (STDMETHODCALLTYPE *get_ZoomFactor)(ICoreWebView2Controller*, double*);
    HRESULT (STDMETHODCALLTYPE *put_ZoomFactor)(ICoreWebView2Controller*, double);
    HRESULT (STDMETHODCALLTYPE *add_ZoomFactorChanged)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ZoomFactorChanged)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *SetBoundsAndZoomFactor)(ICoreWebView2Controller*, RECT, double);
    HRESULT (STDMETHODCALLTYPE *MoveFocus)(ICoreWebView2Controller*, int);
    HRESULT (STDMETHODCALLTYPE *add_MoveFocusRequested)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_MoveFocusRequested)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_GotFocus)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_GotFocus)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_LostFocus)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_LostFocus)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_AcceleratorKeyPressed)(ICoreWebView2Controller*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_AcceleratorKeyPressed)(ICoreWebView2Controller*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_ParentWindow)(ICoreWebView2Controller*, HWND*);
    HRESULT (STDMETHODCALLTYPE *put_ParentWindow)(ICoreWebView2Controller*, HWND);
    HRESULT (STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *Close)(ICoreWebView2Controller*);
    HRESULT (STDMETHODCALLTYPE *get_CoreWebView2)(ICoreWebView2Controller*, ICoreWebView2**);
} ICoreWebView2ControllerVtbl;

struct ICoreWebView2Controller { const ICoreWebView2ControllerVtbl *lpVtbl; };

// ICoreWebView2 vtable (full table required)
typedef struct ICoreWebView2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *get_Settings)(ICoreWebView2*, ICoreWebView2Settings**);
    HRESULT (STDMETHODCALLTYPE *get_Source)(ICoreWebView2*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *Navigate)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *NavigateToString)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *add_NavigationStarting)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationStarting)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ContentLoading)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ContentLoading)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_SourceChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_SourceChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_HistoryChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_HistoryChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_NavigationCompleted)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationCompleted)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationStarting)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationStarting)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_FrameNavigationCompleted)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_FrameNavigationCompleted)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ScriptDialogOpening)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ScriptDialogOpening)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_PermissionRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_PermissionRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_ProcessFailed)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ProcessFailed)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddScriptToExecuteOnDocumentCreated)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *RemoveScriptToExecuteOnDocumentCreated)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *ExecuteScript)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *CapturePreview)(ICoreWebView2*, int, void*, void*);
    HRESULT (STDMETHODCALLTYPE *Reload)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsJson)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *PostWebMessageAsString)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *add_WebMessageReceived)(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventHandler*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WebMessageReceived)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *CallDevToolsProtocolMethod)(ICoreWebView2*, LPCWSTR, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *get_BrowserProcessId)(ICoreWebView2*, UINT32*);
    HRESULT (STDMETHODCALLTYPE *get_CanGoBack)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *get_CanGoForward)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *GoBack)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *GoForward)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *GetDevToolsProtocolEventReceiver)(ICoreWebView2*, LPCWSTR, void**);
    HRESULT (STDMETHODCALLTYPE *Stop)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *add_NewWindowRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_NewWindowRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *add_DocumentTitleChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_DocumentTitleChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_DocumentTitle)(ICoreWebView2*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *AddHostObjectToScript)(ICoreWebView2*, LPCWSTR, void*);
    HRESULT (STDMETHODCALLTYPE *RemoveHostObjectFromScript)(ICoreWebView2*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *OpenDevToolsWindow)(ICoreWebView2*);
    HRESULT (STDMETHODCALLTYPE *add_ContainsFullScreenElementChanged)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_ContainsFullScreenElementChanged)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *get_ContainsFullScreenElement)(ICoreWebView2*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *add_WebResourceRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WebResourceRequested)(ICoreWebView2*, EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *AddWebResourceRequestedFilter)(ICoreWebView2*, LPCWSTR, int);
    HRESULT (STDMETHODCALLTYPE *RemoveWebResourceRequestedFilter)(ICoreWebView2*, LPCWSTR, int);
    HRESULT (STDMETHODCALLTYPE *add_WindowCloseRequested)(ICoreWebView2*, void*, EventRegistrationToken*);
    HRESULT (STDMETHODCALLTYPE *remove_WindowCloseRequested)(ICoreWebView2*, EventRegistrationToken);
} ICoreWebView2Vtbl;

struct ICoreWebView2 { const ICoreWebView2Vtbl *lpVtbl; };

// ICoreWebView2Settings vtable
typedef struct ICoreWebView2SettingsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2Settings*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2Settings*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2Settings*);
    HRESULT (STDMETHODCALLTYPE *get_IsScriptEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsScriptEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsWebMessageEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsWebMessageEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultScriptDialogsEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsStatusBarEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsStatusBarEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDevToolsEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDevToolsEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreDefaultContextMenusEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreDefaultContextMenusEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_AreHostObjectsAllowed)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_AreHostObjectsAllowed)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsZoomControlEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsZoomControlEnabled)(ICoreWebView2Settings*, BOOL);
    HRESULT (STDMETHODCALLTYPE *get_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings*, BOOL*);
    HRESULT (STDMETHODCALLTYPE *put_IsBuiltInErrorPageEnabled)(ICoreWebView2Settings*, BOOL);
} ICoreWebView2SettingsVtbl;

struct ICoreWebView2Settings { const ICoreWebView2SettingsVtbl *lpVtbl; };

// ICoreWebView2WebMessageReceivedEventArgs vtable
typedef struct ICoreWebView2WebMessageReceivedEventArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebMessageReceivedEventArgs*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebMessageReceivedEventArgs*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebMessageReceivedEventArgs*);
    HRESULT (STDMETHODCALLTYPE *get_Source)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *get_WebMessageAsJson)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *TryGetWebMessageAsString)(ICoreWebView2WebMessageReceivedEventArgs*, LPWSTR*);
} ICoreWebView2WebMessageReceivedEventArgsVtbl;

struct ICoreWebView2WebMessageReceivedEventArgs { const ICoreWebView2WebMessageReceivedEventArgsVtbl *lpVtbl; };

// ============================================================================
// COM callback handler types
// ============================================================================

typedef struct EnvironmentCompletedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, HRESULT, ICoreWebView2Environment*);
} EnvironmentCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    const EnvironmentCompletedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

typedef struct ControllerCompletedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, HRESULT, ICoreWebView2Controller*);
} ControllerCompletedHandlerVtbl;

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    const ControllerCompletedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

typedef struct WebMessageReceivedHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ICoreWebView2WebMessageReceivedEventHandler*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ICoreWebView2WebMessageReceivedEventHandler*);
    ULONG   (STDMETHODCALLTYPE *Release)(ICoreWebView2WebMessageReceivedEventHandler*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(ICoreWebView2WebMessageReceivedEventHandler*, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*);
} WebMessageReceivedHandlerVtbl;

struct ICoreWebView2WebMessageReceivedEventHandler {
    const WebMessageReceivedHandlerVtbl *lpVtbl;
    ULONG refCount;
};

// ============================================================================
// WebView2 globals
// ============================================================================

static HWND g_webviewHwnd = NULL;
static ICoreWebView2Environment *g_webviewEnv = NULL;
static ICoreWebView2Controller *g_webviewController = NULL;
static ICoreWebView2 *g_webviewView = NULL;
static BOOL g_webviewWindowShown = FALSE;
static BOOL g_configChanged = FALSE;

typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder, void* options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

static PFN_CreateCoreWebView2EnvironmentWithOptions fnCreateEnvironment = NULL;
static WCHAR g_extractedDllPath[MAX_PATH] = {0};

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
static void InstallWinEventHook(void);
static void RemoveWinEventHook(void);

// Status
static BOOL CheckChromeApiStatus(void);
static void BringChromeToFront(void);
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
        // Find the existing instance's window and tell it to show Chrome
        HWND hwndExisting = FindWindowW(L"ChromeDevLauncherClass", NULL);
        if (hwndExisting) {
            PostMessage(hwndExisting, WM_BRING_CHROME_TO_FRONT, 0, 0);
        }
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

    SID_IDENTIFIER_AUTHORITY ntAuthority = { SECURITY_NT_AUTHORITY };
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
// WebView2 Helper Functions
// ============================================================================

static BOOL load_webview2_loader(void) {
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_WEBVIEW2_DLL), RT_RCDATA);
    if (!hRes) {
        MessageBoxW(NULL, L"Failed to find WebView2Loader.dll in embedded resources.\n"
            L"The executable may need to be rebuilt.", L"Chrome Developer Launcher", MB_ICONERROR);
        return FALSE;
    }
    HGLOBAL hData = LoadResource(NULL, hRes);
    DWORD dllSize = SizeofResource(NULL, hRes);
    const void *dllBytes = LockResource(hData);
    if (!dllBytes || dllSize == 0) {
        MessageBoxW(NULL, L"Failed to load WebView2Loader.dll from embedded resources.",
            L"Chrome Developer Launcher", MB_ICONERROR);
        return FALSE;
    }
    WCHAR tempDir[MAX_PATH];
    DWORD tempLen = GetTempPathW(MAX_PATH, tempDir);
    if (tempLen == 0 || tempLen >= MAX_PATH - 50) {
        MessageBoxW(NULL, L"Failed to get temp directory path.", L"Chrome Developer Launcher", MB_ICONERROR);
        return FALSE;
    }
    // Use a ChromeDevLauncher-specific subdirectory to avoid conflicts
    swprintf(g_extractedDllPath, MAX_PATH, L"%sChromeDevLauncher", tempDir);
    CreateDirectoryW(g_extractedDllPath, NULL);
    swprintf(g_extractedDllPath, MAX_PATH, L"%sChromeDevLauncher\\WebView2Loader.dll", tempDir);

    // Try to load existing copy first (may already be extracted from a previous run)
    HMODULE hMod = LoadLibraryW(g_extractedDllPath);
    if (!hMod) {
        // Extract fresh copy
        HANDLE hFile = CreateFileW(g_extractedDllPath, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            WCHAR msg[512];
            swprintf(msg, 512, L"Failed to write WebView2Loader.dll to temp directory.\n\n"
                L"Path: %s\nError: %lu", g_extractedDllPath, GetLastError());
            MessageBoxW(NULL, msg, L"Chrome Developer Launcher", MB_ICONERROR);
            return FALSE;
        }
        DWORD written = 0;
        WriteFile(hFile, dllBytes, dllSize, &written, NULL);
        CloseHandle(hFile);
        if (written != dllSize) {
            MessageBoxW(NULL, L"Failed to write complete WebView2Loader.dll to temp directory.",
                L"Chrome Developer Launcher", MB_ICONERROR);
            return FALSE;
        }
        hMod = LoadLibraryW(g_extractedDllPath);
    }
    if (!hMod) {
        WCHAR msg[512];
        swprintf(msg, 512, L"Failed to load WebView2Loader.dll.\n\n"
            L"Path: %s\nError: %lu", g_extractedDllPath, GetLastError());
        MessageBoxW(NULL, msg, L"Chrome Developer Launcher", MB_ICONERROR);
        return FALSE;
    }
    fnCreateEnvironment = (PFN_CreateCoreWebView2EnvironmentWithOptions)
        GetProcAddress(hMod, "CreateCoreWebView2EnvironmentWithOptions");
    if (!fnCreateEnvironment) {
        MessageBoxW(NULL, L"WebView2Loader.dll loaded but CreateCoreWebView2EnvironmentWithOptions not found.\n\n"
            L"The DLL may be corrupted or the wrong version.", L"Chrome Developer Launcher", MB_ICONERROR);
        return FALSE;
    }
    return TRUE;
}

static void webview_execute_script(const wchar_t* script) {
    if (g_webviewView) {
        g_webviewView->lpVtbl->ExecuteScript(g_webviewView, script, NULL);
    }
}

static void webview_sync_controller_bounds(void) {
    if (!g_webviewController || !g_webviewHwnd) return;
    RECT bounds;
    GetClientRect(g_webviewHwnd, &bounds);
    g_webviewController->lpVtbl->put_Bounds(g_webviewController, bounds);
    g_webviewController->lpVtbl->put_IsVisible(g_webviewController, TRUE);
}

// Minimal JSON parser helpers
static BOOL json_get_string(const char *json, const char *key, char *out, size_t outLen) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return FALSE;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return FALSE;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outLen - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return TRUE;
}

static BOOL json_get_int(const char *json, const char *key, int *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return FALSE;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    *out = atoi(p);
    return TRUE;
}

// Escape a wide-char string for safe JSON embedding
static void json_escape_wstring(const wchar_t *in, wchar_t *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < outLen - 2; i++) {
        wchar_t c = in[i];
        if (c == L'"' || c == L'\\') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = c;
        } else if (c == L'\n') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = L'n';
        } else if (c == L'\r') {
            if (j + 2 >= outLen) break;
            out[j++] = L'\\';
            out[j++] = L'r';
        } else {
            out[j++] = c;
        }
    }
    out[j] = L'\0';
}

// ============================================================================
// Push functions (C -> JS)
// ============================================================================

static void webview_push_init_config(void) {
    wchar_t wPath[MAX_PATH * 2];
    json_escape_wstring(g_config.chromePath, wPath, MAX_PATH * 2);
    wchar_t wAddr[128];
    json_escape_wstring(g_config.connectAddress, wAddr, 128);
    wchar_t script[4096];
    swprintf(script, 4096,
        L"window.onInit({\"view\":\"config\",\"config\":{\"chromePath\":\"%s\",\"debugPort\":%d,\"connectAddress\":\"%s\",\"statusCheckInterval\":%d}})",
        wPath, g_config.debugPort, wAddr, g_config.statusCheckInterval);
    webview_execute_script(script);
}

static void webview_push_browse_result(const wchar_t* path) {
    wchar_t wPath[MAX_PATH * 2];
    json_escape_wstring(path, wPath, MAX_PATH * 2);
    wchar_t script[MAX_PATH * 2 + 128];
    swprintf(script, sizeof(script) / sizeof(wchar_t),
        L"window.onBrowseResult({\"path\":\"%s\"})", wPath);
    webview_execute_script(script);
}

// ============================================================================
// COM callback handler implementations
// ============================================================================

static HRESULT STDMETHODCALLTYPE EnvCompleted_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, HRESULT, ICoreWebView2Environment*);
static HRESULT STDMETHODCALLTYPE CtrlCompleted_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, HRESULT, ICoreWebView2Controller*);
static HRESULT STDMETHODCALLTYPE MsgReceived_Invoke(ICoreWebView2WebMessageReceivedEventHandler*, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*);

static HRESULT STDMETHODCALLTYPE EnvCompleted_QueryInterface(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, REFIID riid, void **ppv) {
    (void)riid;
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE EnvCompleted_AddRef(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    return ++This->refCount;
}
static ULONG STDMETHODCALLTYPE EnvCompleted_Release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    ULONG rc = --This->refCount;
    if (rc == 0) free(This);
    return rc;
}

static HRESULT STDMETHODCALLTYPE EnvCompleted_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT result, ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(result) || !env) return result;
    g_webviewEnv = env;
    env->lpVtbl->AddRef(env);

    static ControllerCompletedHandlerVtbl ctrlVtbl = {0};
    static BOOL ctrlVtblInit = FALSE;
    if (!ctrlVtblInit) {
        ctrlVtbl.QueryInterface = (HRESULT (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, REFIID, void**))EnvCompleted_QueryInterface;
        ctrlVtbl.AddRef = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*))EnvCompleted_AddRef;
        ctrlVtbl.Release = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*))EnvCompleted_Release;
        ctrlVtbl.Invoke = CtrlCompleted_Invoke;
        ctrlVtblInit = TRUE;
    }

    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler = malloc(sizeof(*handler));
    handler->lpVtbl = &ctrlVtbl;
    handler->refCount = 1;

    env->lpVtbl->CreateCoreWebView2Controller(env, g_webviewHwnd, handler);
    handler->lpVtbl->Release(handler);
    return S_OK;
}

static EnvironmentCompletedHandlerVtbl g_envCompletedVtbl = {
    EnvCompleted_QueryInterface,
    EnvCompleted_AddRef,
    EnvCompleted_Release,
    EnvCompleted_Invoke
};

static HRESULT STDMETHODCALLTYPE CtrlCompleted_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT result, ICoreWebView2Controller *controller) {
    (void)This;
    if (FAILED(result) || !controller) return result;

    g_webviewController = controller;
    controller->lpVtbl->AddRef(controller);

    RECT bounds;
    GetClientRect(g_webviewHwnd, &bounds);
    controller->lpVtbl->put_Bounds(controller, bounds);
    controller->lpVtbl->put_IsVisible(controller, TRUE);

    ICoreWebView2 *webview = NULL;
    controller->lpVtbl->get_CoreWebView2(controller, &webview);
    if (!webview) return E_FAIL;
    g_webviewView = webview;

    ICoreWebView2Settings *settings = NULL;
    webview->lpVtbl->get_Settings(webview, &settings);
    if (settings) {
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings, FALSE);
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_IsZoomControlEnabled(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }

    static WebMessageReceivedHandlerVtbl msgVtbl = {0};
    static BOOL msgVtblInit = FALSE;
    if (!msgVtblInit) {
        msgVtbl.QueryInterface = (HRESULT (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*, REFIID, void**))EnvCompleted_QueryInterface;
        msgVtbl.AddRef = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*))EnvCompleted_AddRef;
        msgVtbl.Release = (ULONG (STDMETHODCALLTYPE *)(ICoreWebView2WebMessageReceivedEventHandler*))EnvCompleted_Release;
        msgVtbl.Invoke = MsgReceived_Invoke;
        msgVtblInit = TRUE;
    }

    ICoreWebView2WebMessageReceivedEventHandler *msgHandler = malloc(sizeof(*msgHandler));
    msgHandler->lpVtbl = &msgVtbl;
    msgHandler->refCount = 1;

    EventRegistrationToken token;
    webview->lpVtbl->add_WebMessageReceived(webview, msgHandler, &token);
    msgHandler->lpVtbl->Release(msgHandler);

    // Load embedded HTML from resources
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_HTML_UI), RT_RCDATA);
    if (hRes) {
        HGLOBAL hData = LoadResource(NULL, hRes);
        if (hData) {
            DWORD htmlSize = SizeofResource(NULL, hRes);
            const char *htmlUtf8 = (const char *)LockResource(hData);
            if (htmlUtf8 && htmlSize > 0) {
                int wLen = MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, (int)htmlSize, NULL, 0);
                wchar_t *wHtml = malloc((wLen + 1) * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, htmlUtf8, (int)htmlSize, wHtml, wLen);
                wHtml[wLen] = L'\0';
                webview->lpVtbl->NavigateToString(webview, wHtml);
                free(wHtml);
            }
        }
    }

    return S_OK;
}

// --- WebMessageReceivedHandler ---

static HRESULT STDMETHODCALLTYPE MsgReceived_Invoke(ICoreWebView2WebMessageReceivedEventHandler *This, ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)This; (void)sender;

    LPWSTR wMsg = NULL;
    args->lpVtbl->TryGetWebMessageAsString(args, &wMsg);
    if (!wMsg) return S_OK;

    int len = WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, NULL, 0, NULL, NULL);
    char *msg = malloc(len);
    WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, msg, len, NULL, NULL);
    CoTaskMemFree(wMsg);

    char action[64] = {0};
    json_get_string(msg, "action", action, sizeof(action));

    if (strcmp(action, "getInit") == 0) {
        webview_push_init_config();
    } else if (strcmp(action, "saveSettings") == 0) {
        char chromePath[MAX_PATH] = {0};
        char connectAddress[64] = {0};
        int debugPort = 9222;
        int statusCheckInterval = 60;

        json_get_string(msg, "chromePath", chromePath, sizeof(chromePath));
        json_get_string(msg, "connectAddress", connectAddress, sizeof(connectAddress));
        json_get_int(msg, "debugPort", &debugPort);
        json_get_int(msg, "statusCheckInterval", &statusCheckInterval);

        // Write to global config
        MultiByteToWideChar(CP_UTF8, 0, chromePath, -1, g_config.chromePath, MAX_PATH);
        MultiByteToWideChar(CP_UTF8, 0, connectAddress, -1, g_config.connectAddress, 64);
        g_config.debugPort = debugPort;
        g_config.statusCheckInterval = statusCheckInterval;
        g_configChanged = TRUE;

        PostMessage(g_webviewHwnd, WM_CLOSE, 0, 0);
    } else if (strcmp(action, "browse") == 0) {
        OPENFILENAMEW ofn = {0};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_webviewHwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = L"Select Chrome Executable";
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            webview_push_browse_result(szFile);
        }
    } else if (strcmp(action, "close") == 0) {
        PostMessage(g_webviewHwnd, WM_CLOSE, 0, 0);
    } else if (strcmp(action, "resize") == 0) {
        int contentHeight = 0;
        json_get_int(msg, "height", &contentHeight);
        if (contentHeight > 0 && g_webviewHwnd) {
            RECT clientRect = {0}, windowRect = {0};
            GetClientRect(g_webviewHwnd, &clientRect);
            GetWindowRect(g_webviewHwnd, &windowRect);
            int chromeH = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
            int newWindowH = contentHeight + chromeH;
            int windowW = windowRect.right - windowRect.left;
            UINT flags = SWP_NOMOVE | SWP_NOZORDER;
            if (g_webviewWindowShown) {
                flags |= SWP_NOACTIVATE;
            } else {
                flags |= SWP_SHOWWINDOW;
                KillTimer(g_webviewHwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
            }
            SetWindowPos(g_webviewHwnd, NULL, 0, 0, windowW, newWindowH, flags);
            g_webviewWindowShown = TRUE;
            webview_sync_controller_bounds();
        }
    }

    free(msg);
    return S_OK;
}

// ============================================================================
// WebView2 window
// ============================================================================

static LRESULT CALLBACK WebViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            webview_sync_controller_bounds();
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_WEBVIEW_SHOW_FALLBACK) {
                KillTimer(hwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
                if (!g_webviewWindowShown) {
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    UpdateWindow(hwnd);
                    g_webviewWindowShown = TRUE;
                    webview_sync_controller_bounds();
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            g_webviewWindowShown = FALSE;
            KillTimer(hwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
            if (g_webviewController) {
                g_webviewController->lpVtbl->Close(g_webviewController);
                g_webviewController->lpVtbl->Release(g_webviewController);
                g_webviewController = NULL;
            }
            if (g_webviewView) {
                g_webviewView->lpVtbl->Release(g_webviewView);
                g_webviewView = NULL;
            }
            if (g_webviewEnv) {
                g_webviewEnv->lpVtbl->Release(g_webviewEnv);
                g_webviewEnv = NULL;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_webviewHwnd = NULL;
            g_webviewWindowShown = FALSE;
            KillTimer(hwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowWebViewDialog(int width, int height) {
    // If already open, bring to front
    if (g_webviewHwnd != NULL) {
        SetForegroundWindow(g_webviewHwnd);
        return;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!fnCreateEnvironment && !load_webview2_loader()) {
        return;
    }

    // Register window class (once)
    static BOOL classRegistered = FALSE;
    if (!classRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WebViewWndProc;
        wc.hInstance = g_hInstance;
        wc.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ChromeDevLauncherWebViewWnd";
        wc.hIconSm = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
        RegisterClassExW(&wc);
        classRegistered = TRUE;
    }

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - width) / 2;
    int posY = (screenH - height) / 2;

    g_webviewHwnd = CreateWindowExW(0, L"ChromeDevLauncherWebViewWnd", L"Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, width, height,
        NULL, NULL, g_hInstance, NULL);

    if (!g_webviewHwnd) {
        return;
    }
    g_webviewWindowShown = FALSE;
    SetTimer(g_webviewHwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK, WEBVIEW_SHOW_FALLBACK_DELAY_MS, NULL);

    // Build user data folder path
    WCHAR userDataFolder[MAX_PATH];
    DWORD tempLen = GetTempPathW(MAX_PATH, userDataFolder);
    if (tempLen > 0 && tempLen < MAX_PATH - 30) {
        wcscat(userDataFolder, L"ChromeDevLauncher.WebView2");
    } else {
        wcscpy(userDataFolder, L"");
    }

    // Create environment
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *envHandler = malloc(sizeof(*envHandler));
    envHandler->lpVtbl = &g_envCompletedVtbl;
    envHandler->refCount = 1;

    HRESULT hr = fnCreateEnvironment(NULL, userDataFolder[0] ? userDataFolder : NULL, NULL, envHandler);
    envHandler->lpVtbl->Release(envHandler);

    if (FAILED(hr)) {
        MessageBoxW(NULL,
            L"Failed to initialize WebView2.\n\n"
            L"Please ensure the Microsoft Edge WebView2 Runtime is installed.\n"
            L"Download from: https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
            L"Chrome Developer Launcher", MB_ICONERROR | MB_OK);
        DestroyWindow(g_webviewHwnd);
        g_webviewHwnd = NULL;
    }
}

// ============================================================================
// Configuration Dialog
// ============================================================================

static BOOL ShowConfigDialog(HWND hwndParent) {
    (void)hwndParent;

    // Save copy of current config for comparison after dialog closes
    Configuration savedConfig;
    memcpy(&savedConfig, &g_config, sizeof(Configuration));
    g_configChanged = FALSE;

    // Show WebView2 dialog
    ShowWebViewDialog(480, 340);

    // Run local message loop until WebView2 window is destroyed (makes call blocking)
    MSG msg;
    while (g_webviewHwnd && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_configChanged) {
        // Check if Chrome-impacting settings changed
        BOOL needsRestart = FALSE;
        if (wcscmp(savedConfig.chromePath, g_config.chromePath) != 0 ||
            savedConfig.debugPort != g_config.debugPort ||
            wcscmp(savedConfig.connectAddress, g_config.connectAddress) != 0) {
            needsRestart = TRUE;
        }

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

    // Use fixed directory name for persistent profile
    swprintf_s(g_szTempDir, MAX_PATH, L"%schrome_debug", tempPath);

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

    // Build command line - start off-screen so window is never visible
    wchar_t cmdLine[MAX_PATH * 2];
    swprintf_s(cmdLine, sizeof(cmdLine)/sizeof(wchar_t),
               L"\"%s\" --remote-debugging-port=%d --user-data-dir=\"%s\" --window-position=-32000,-32000",
               g_config.chromePath, g_config.debugPort, g_szTempDir);

    // Launch Chrome
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // Start completely hidden

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
    g_chromeHidden = TRUE;  // Start hidden on every launch

    // Install real-time hook to catch any new windows
    InstallWinEventHook();

    CloseHandle(pi.hThread);

    return TRUE;
}

static void TerminateChrome(void) {
    // Remove window event hook
    RemoveWinEventHook();

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
    // Profile directory is intentionally kept for persistence
}

static void RestartChrome(void) {
    TerminateChrome();
    Sleep(500);  // Brief pause
    SetupPortForwards();
    LaunchChrome();
}

// ============================================================================
// Chrome Taskbar Hiding
// ============================================================================

// Real-time hook callback - fires when any window is shown
static void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime)
{
    (void)hWinEventHook;
    (void)event;
    (void)idChild;
    (void)idEventThread;
    (void)dwmsEventTime;

    // Only care about window objects
    if (idObject != OBJID_WINDOW || !hwnd) return;
    if (!g_chromeHidden || !g_hJob) return;

    // Check if this window belongs to our Chrome job
    DWORD windowPID;
    GetWindowThreadProcessId(hwnd, &windowPID);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, windowPID);
    if (hProcess) {
        BOOL isInJob = FALSE;
        IsProcessInJob(hProcess, g_hJob, &isInJob);
        CloseHandle(hProcess);

        if (isInJob) {
            wchar_t className[256];
            GetClassNameW(hwnd, className, 256);
            if (wcscmp(className, L"Chrome_WidgetWin_1") == 0) {
                // Move off-screen immediately, then hide
                SetWindowPos(hwnd, NULL, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(hwnd, SW_HIDE);
                LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                if (!(exStyle & WS_EX_TOOLWINDOW)) {
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW);
                }
            }
        }
    }
}

static void InstallWinEventHook(void) {
    if (g_hWinEventHook) return;  // Already installed

    g_hWinEventHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,  // Only catch show events
        NULL,                                   // No DLL
        WinEventProc,                          // Callback
        0,                                     // All processes
        0,                                     // All threads
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
}

static void RemoveWinEventHook(void) {
    if (g_hWinEventHook) {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = NULL;
    }
}

// ============================================================================
// Chrome Window Management
// ============================================================================

static HWND g_lastChromeWindow = NULL;  // Track last window for SetForegroundWindow

static BOOL CALLBACK RestoreChromeWindowsProc(HWND hwnd, LPARAM lParam) {
    (void)lParam;

    DWORD windowPID;
    GetWindowThreadProcessId(hwnd, &windowPID);

    if (g_hJob) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, windowPID);
        if (hProcess) {
            BOOL isInJob = FALSE;
            IsProcessInJob(hProcess, g_hJob, &isInJob);
            CloseHandle(hProcess);

            if (isInJob) {
                wchar_t className[256];
                GetClassNameW(hwnd, className, 256);
                if (wcscmp(className, L"Chrome_WidgetWin_1") == 0) {
                    // Check if window is off-screen and move it back
                    RECT rect;
                    if (GetWindowRect(hwnd, &rect)) {
                        if (rect.left < -10000 || rect.top < -10000) {
                            // Move to center of screen
                            int screenW = GetSystemMetrics(SM_CXSCREEN);
                            int screenH = GetSystemMetrics(SM_CYSCREEN);
                            int winW = rect.right - rect.left;
                            int winH = rect.bottom - rect.top;
                            int x = (screenW - winW) / 2;
                            int y = (screenH - winH) / 2;
                            SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                        }
                    }

                    // Show and restore the window
                    ShowWindow(hwnd, SW_SHOW);
                    if (IsIconic(hwnd)) {
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                    // Track window with title as main window
                    wchar_t title[256];
                    if (GetWindowTextW(hwnd, title, 256) > 0) {
                        g_lastChromeWindow = hwnd;
                    }
                }
            }
        }
    }
    return TRUE;  // Continue enumeration to restore all windows
}

static void BringChromeToFront(void) {
    if (!g_chromeRunning || !g_hJob) return;

    // Mark as no longer hidden
    g_chromeHidden = FALSE;

    // Restore all Chrome windows
    g_lastChromeWindow = NULL;
    EnumWindows(RestoreChromeWindowsProc, 0);

    // Bring the main window to front
    if (g_lastChromeWindow) {
        SetForegroundWindow(g_lastChromeWindow);
    }
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
    // Close WebView2 dialog if open
    if (g_webviewHwnd) SendMessage(g_webviewHwnd, WM_CLOSE, 0, 0);

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
                if (g_hJob && g_chromeRunning) {
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION jobInfo;
                    if (QueryInformationJobObject(g_hJob, JobObjectBasicAccountingInformation,
                                                   &jobInfo, sizeof(jobInfo), NULL)) {
                        if (jobInfo.ActiveProcesses == 0) {
                            // All processes in the job have exited
                            TerminateChrome();

                            // Always attempt relaunch with fresh firewall rules
                            // First validate the chrome path exists
                            BOOL canRelaunch = FALSE;
                            if (g_config.chromePath[0] != L'\0') {
                                DWORD attrs = GetFileAttributesW(g_config.chromePath);
                                if (attrs != INVALID_FILE_ATTRIBUTES &&
                                    !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                                    canRelaunch = TRUE;
                                }
                            }

                            if (canRelaunch) {
                                Sleep(500);
                                // Reset firewall rules (remove and re-add)
                                CleanupAllPortForwards();
                                SetupPortForwards();
                                if (!LaunchChrome()) {
                                    // Launch failed - don't retry
                                    CleanupAllPortForwards();
                                }
                            }

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
                    BringChromeToFront();
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

        case WM_BRING_CHROME_TO_FRONT:
            BringChromeToFront();
            return 0;

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
