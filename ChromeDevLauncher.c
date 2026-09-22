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
#include <userenv.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <winver.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <objbase.h>

#include "version.h"

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
#define REG_VALUE_AUTO_UPDATE L"AutoCheckForUpdates"
#define REG_VALUE_IGNORED_UPDATE_VERSION L"IgnoredUpdateVersion"
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
#define WM_APP_UPDATE_RESULT (WM_APP + 2)
#define WM_APP_UPDATE_PROGRESS (WM_APP + 3)

// Resource IDs
#define IDR_HTML_UI      200
#define IDR_WEBVIEW2_DLL 201

// Timers
#define ID_TIMER_STATUS_CHECK 1
#define ID_TIMER_CHROME_EXIT 2
#define ID_TIMER_AUTO_UPDATE 3
#define CHROME_EXIT_CHECK_INTERVAL 1000
#define AUTO_UPDATE_INTERVAL_MS (60u * 60u * 1000u)
#define ID_TIMER_WEBVIEW_SHOW_FALLBACK 1006
#define WEBVIEW_SHOW_FALLBACK_DELAY_MS 350

// Self update
#define UPDATE_URL L"https://github.com/JPITSG/ChromeDevLauncher/raw/refs/heads/main/release/ChromeDevLauncher.exe"
#define UPDATE_MAX_BYTES (100ULL * 1024ULL * 1024ULL)
#define UPDATE_HELPER_READY_MS 10000
#define UPDATE_HELPER_WAIT_MS 120000

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
    BOOL autoCheckForUpdates;
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

typedef struct {
    WORD major;
    WORD minor;
    WORD patch;
    WORD build;
} ExecutableVersion;

typedef enum {
    UPDATE_CHECK_SAME = 1,
    UPDATE_CHECK_NEWER,
    UPDATE_CHECK_OLDER,
    UPDATE_CHECK_CANCELLED,
    UPDATE_CHECK_ERROR
} UpdateCheckKind;

typedef struct {
    HWND targetWindow;
    BOOL automatic;
    UpdateCheckKind kind;
    ULONGLONG cacheBuster;
    ExecutableVersion runningVersion;
    ExecutableVersion availableVersion;
    wchar_t message[512];
    wchar_t targetPath[MAX_PATH];
    wchar_t stagedPath[MAX_PATH];
} UpdateCheckTask;

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

// Self update
static volatile LONG g_updateCheckPending = FALSE;
static volatile LONG g_updateCheckAutomatic = FALSE;
static volatile LONG g_updateRequestSequence = 0;
static volatile LONG g_updateProgressPercent = -1; // -1 until the download starts
static volatile LONG g_updateProgressPosted = FALSE;
static HANDLE g_updateCancelEvent = NULL;
static UpdateCheckTask* volatile g_updatePostedResult = NULL;
static UpdateCheckTask* g_updateNoticeTask = NULL;
static UpdateCheckTask* g_updateReadyTask = NULL;
static wchar_t g_ignoredUpdateVersion[32] = L"";
static BOOL g_updateInstallReady = FALSE;
static BOOL g_updateConfirmationPending = FALSE;

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
static BOOL g_configViewReady = FALSE;

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

// Self update
static void StartUpdateCheck(BOOL automatic);
static void CancelUpdateCheck(void);
static void InstallPreparedUpdate(BOOL reopenSettings);
static void DiscardPreparedUpdate(void);

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
        // Preserve the one-time updater handoff if startup needs elevation.
        sei.lpParameters = PathGetArgsW(GetCommandLineW());
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
    config->autoCheckForUpdates = TRUE;
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

    // Automatic update checks (enabled by default)
    DWORD autoUpdateValue = 1;
    dataSize = sizeof(autoUpdateValue);
    if (RegQueryValueExW(hKey, REG_VALUE_AUTO_UPDATE, NULL, &dataType,
                         (LPBYTE)&autoUpdateValue, &dataSize) == ERROR_SUCCESS) {
        config->autoCheckForUpdates = (autoUpdateValue != 0);
    }

    dataSize = sizeof(g_ignoredUpdateVersion);
    if (RegQueryValueExW(hKey, REG_VALUE_IGNORED_UPDATE_VERSION, NULL,
                         &dataType, (LPBYTE)g_ignoredUpdateVersion,
                         &dataSize) != ERROR_SUCCESS ||
        dataType != REG_SZ || dataSize < sizeof(wchar_t)) {
        g_ignoredUpdateVersion[0] = L'\0';
    }
    g_ignoredUpdateVersion[
        (sizeof(g_ignoredUpdateVersion) / sizeof(wchar_t)) - 1] = L'\0';

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

    // Automatic update checks and the version ignored by automatic prompts
    DWORD autoUpdateValue = config->autoCheckForUpdates ? 1 : 0;
    RegSetValueExW(hKey, REG_VALUE_AUTO_UPDATE, 0, REG_DWORD,
                   (const BYTE*)&autoUpdateValue, sizeof(autoUpdateValue));
    RegSetValueExW(hKey, REG_VALUE_IGNORED_UPDATE_VERSION, 0, REG_SZ,
                   (const BYTE*)g_ignoredUpdateVersion,
                   (DWORD)((wcslen(g_ignoredUpdateVersion) + 1) *
                           sizeof(wchar_t)));

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
    while (*p && i < outLen - 1) {
        if (*p == '"') break;
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
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

static BOOL json_get_bool(const char *json, const char *key, BOOL defaultValue) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return defaultValue;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return TRUE;
    if (strncmp(p, "false", 5) == 0) return FALSE;
    if (*p == '1') return TRUE;
    if (*p == '0') return FALSE;
    return defaultValue;
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
// Self Update
// ============================================================================

typedef struct {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
} UpdateHttpRequest;

static void CloseUpdateHttpRequest(UpdateHttpRequest* http) {
    if (!http) return;
    if (http->request) WinHttpCloseHandle(http->request);
    if (http->connection) WinHttpCloseHandle(http->connection);
    if (http->session) WinHttpCloseHandle(http->session);
    ZeroMemory(http, sizeof(*http));
}

static void SetUpdateTaskError(UpdateCheckTask* task, LPCWSTR message,
                               DWORD errorCode) {
    if (!task) return;
    task->kind = UPDATE_CHECK_ERROR;
    if (errorCode) {
        swprintf_s(task->message, sizeof(task->message) / sizeof(wchar_t),
                   L"%s (Windows error %lu).", message,
                   (unsigned long)errorCode);
    } else {
        wcscpy_s(task->message, sizeof(task->message) / sizeof(wchar_t), message);
    }
}

static BOOL CancelUpdateTaskIfRequested(UpdateCheckTask* task) {
    if (!task || !g_updateCancelEvent ||
        WaitForSingleObject(g_updateCancelEvent, 0) != WAIT_OBJECT_0) {
        return FALSE;
    }
    task->kind = UPDATE_CHECK_CANCELLED;
    task->message[0] = L'\0';
    return TRUE;
}

// Round down to whole percent so 100 appears only once every byte has
// arrived. Download sizes are bounded by UPDATE_MAX_BYTES.
static DWORD CalculateUpdateProgressPercent(ULONGLONG receivedBytes,
                                            ULONGLONG totalBytes) {
    if (!totalBytes) return 0;
    if (receivedBytes >= totalBytes) return 100;
    return (DWORD)(receivedBytes * 100ULL / totalBytes);
}

static void PublishUpdateProgress(UpdateCheckTask* task, DWORD percent) {
    if (!task || CancelUpdateTaskIfRequested(task) ||
        !IsWindow(task->targetWindow)) {
        return;
    }

    InterlockedExchange(&g_updateProgressPercent, (LONG)percent);
    if (InterlockedCompareExchange(&g_updateProgressPosted, TRUE, FALSE) == FALSE &&
        !PostMessageW(task->targetWindow, WM_APP_UPDATE_PROGRESS, 0, 0)) {
        InterlockedExchange(&g_updateProgressPosted, FALSE);
    }
}

static BOOL OpenUpdateHttpRequest(LPCWSTR verb, ULONGLONG cacheBuster,
                                  UpdateHttpRequest* http, DWORD* statusCode) {
    if (!verb || !http) return FALSE;
    ZeroMemory(http, sizeof(*http));
    if (statusCode) *statusCode = 0;

    wchar_t hostName[256] = L"";
    wchar_t urlPath[2048] = L"";
    wchar_t extraInfo[512] = L"";
    URL_COMPONENTS components = {0};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = hostName;
    components.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);
    components.lpszExtraInfo = extraInfo;
    components.dwExtraInfoLength = sizeof(extraInfo) / sizeof(wchar_t);
    if (!WinHttpCrackUrl(UPDATE_URL, 0, 0, &components)) return FALSE;

    wchar_t objectName[2560];
    if (wcscpy_s(objectName, sizeof(objectName) / sizeof(wchar_t), urlPath) != 0 ||
        wcscat_s(objectName, sizeof(objectName) / sizeof(wchar_t), extraInfo) != 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    // Keep each check independent of proxy and GitHub edge caches. HEAD and
    // GET share one value so a changing artifact is detected by its size.
    wchar_t cacheSuffix[64];
    wchar_t separator = wcschr(objectName, L'?') ? L'&' : L'?';
    int suffixLength = swprintf_s(cacheSuffix,
        sizeof(cacheSuffix) / sizeof(wchar_t), L"%lccdlUpdate=%016llx",
        separator, (unsigned long long)cacheBuster);
    if (suffixLength <= 0 ||
        wcscat_s(objectName, sizeof(objectName) / sizeof(wchar_t),
                 cacheSuffix) != 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    http->session = WinHttpOpen(L"ChromeDevLauncher Update",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http->session) goto fail;
    WinHttpSetTimeouts(http->session, 10000, 10000, 15000, 30000);

    http->connection = WinHttpConnect(http->session, hostName,
                                      components.nPort, 0);
    if (!http->connection) goto fail;

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (components.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;
    http->request = WinHttpOpenRequest(http->connection, verb, objectName,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!http->request) goto fail;

    static const wchar_t noCacheHeaders[] =
        L"Cache-Control: no-cache, no-store, max-age=0\r\nPragma: no-cache\r\n";
    WinHttpAddRequestHeaders(http->request, noCacheHeaders, (DWORD)-1L,
                            WINHTTP_ADDREQ_FLAG_ADD |
                            WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!WinHttpSendRequest(http->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http->request, NULL)) {
        goto fail;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(http->request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        goto fail;
    }
    if (statusCode) *statusCode = status;
    if (status != 200) {
        CloseUpdateHttpRequest(http);
        SetLastError(ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
        return FALSE;
    }
    return TRUE;

fail: {
        DWORD errorCode = GetLastError();
        CloseUpdateHttpRequest(http);
        SetLastError(errorCode);
        return FALSE;
    }
}

static BOOL QueryUpdateContentLength(HINTERNET request, ULONGLONG* size) {
    if (!request || !size) return FALSE;
    wchar_t lengthText[64] = L"";
    DWORD lengthBytes = sizeof(lengthText);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return FALSE;
    }
    lengthText[(sizeof(lengthText) / sizeof(wchar_t)) - 1] = L'\0';

    wchar_t* end = NULL;
    unsigned long long parsed = _wcstoui64(lengthText, &end, 10);
    if (end == lengthText || !end || *end != L'\0' || parsed == 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *size = (ULONGLONG)parsed;
    return TRUE;
}

static BOOL GetExecutableVersion(LPCWSTR path, ExecutableVersion* version) {
    if (!path || !*path || !version) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD ignored = 0;
    DWORD infoSize = GetFileVersionInfoSizeW(path, &ignored);
    if (infoSize == 0) {
        DWORD errorCode = GetLastError();
        SetLastError(errorCode ? errorCode : ERROR_RESOURCE_DATA_NOT_FOUND);
        return FALSE;
    }

    BYTE* infoData = (BYTE*)malloc(infoSize);
    if (!infoData) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!GetFileVersionInfoW(path, 0, infoSize, infoData)) {
        DWORD errorCode = GetLastError();
        free(infoData);
        SetLastError(errorCode ? errorCode : ERROR_INVALID_DATA);
        return FALSE;
    }

    VS_FIXEDFILEINFO* fixedInfo = NULL;
    UINT fixedInfoSize = 0;
    if (!VerQueryValueW(infoData, L"\\", (LPVOID*)&fixedInfo, &fixedInfoSize) ||
        !fixedInfo || fixedInfoSize < sizeof(*fixedInfo) ||
        fixedInfo->dwSignature != VS_FFI_SIGNATURE) {
        free(infoData);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    version->major = HIWORD(fixedInfo->dwFileVersionMS);
    version->minor = LOWORD(fixedInfo->dwFileVersionMS);
    version->patch = HIWORD(fixedInfo->dwFileVersionLS);
    version->build = LOWORD(fixedInfo->dwFileVersionLS);
    free(infoData);
    return TRUE;
}

static int CompareExecutableVersions(const ExecutableVersion* left,
                                     const ExecutableVersion* right) {
    const WORD leftParts[] = {
        left->major, left->minor, left->patch, left->build
    };
    const WORD rightParts[] = {
        right->major, right->minor, right->patch, right->build
    };
    for (size_t index = 0;
         index < sizeof(leftParts) / sizeof(leftParts[0]); index++) {
        if (leftParts[index] < rightParts[index]) return -1;
        if (leftParts[index] > rightParts[index]) return 1;
    }
    return 0;
}

static void FormatExecutableVersion(const ExecutableVersion* version,
                                    wchar_t* text, size_t textCch) {
    if (!version || !text || textCch == 0) return;
    if (swprintf_s(text, textCch, L"%u.%u.%u.%u",
                   (unsigned int)version->major,
                   (unsigned int)version->minor,
                   (unsigned int)version->patch,
                   (unsigned int)version->build) <= 0) {
        text[0] = L'\0';
    }
}

static void FormatExecutableVersionForDisplay(const ExecutableVersion* version,
                                              wchar_t* text, size_t textCch) {
    if (!version || !text || textCch == 0) return;
    if (version->build == 0) {
        if (swprintf_s(text, textCch, L"%u.%u.%u",
                       (unsigned int)version->major,
                       (unsigned int)version->minor,
                       (unsigned int)version->patch) <= 0) {
            text[0] = L'\0';
        }
        return;
    }
    FormatExecutableVersion(version, text, textCch);
}

static BOOL BuildUpdateTempPath(wchar_t path[MAX_PATH], LPCWSTR role,
                                DWORD processId) {
    if (!path || !role || !*role || processId == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    wchar_t tempDirectory[MAX_PATH];
    DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (tempLength == 0) return FALSE;
    if (tempLength >= MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    int length = swprintf_s(path, MAX_PATH, L"%sChromeDevLauncher-%s-%lu.exe",
                            tempDirectory, role, (unsigned long)processId);
    if (length <= 0 || length >= MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

static BOOL DeleteUpdateTempFile(LPCWSTR path) {
    if (!path || !*path) return FALSE;
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path)) return TRUE;
    DWORD errorCode = GetLastError();
    return errorCode == ERROR_FILE_NOT_FOUND ||
           errorCode == ERROR_PATH_NOT_FOUND;
}

static BOOL QueryRemoteUpdateSize(UpdateCheckTask* task, ULONGLONG* size) {
    if (CancelUpdateTaskIfRequested(task)) return FALSE;

    UpdateHttpRequest http;
    DWORD status = 0;
    if (!OpenUpdateHttpRequest(L"HEAD", task->cacheBuster, &http, &status)) {
        DWORD errorCode = GetLastError();
        if (CancelUpdateTaskIfRequested(task)) return FALSE;
        if (status) {
            swprintf_s(task->message,
                       sizeof(task->message) / sizeof(wchar_t),
                       L"The update server returned HTTP status %lu.",
                       (unsigned long)status);
            task->kind = UPDATE_CHECK_ERROR;
        } else {
            SetUpdateTaskError(task,
                L"Could not contact the update server", errorCode);
        }
        return FALSE;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        CloseUpdateHttpRequest(&http);
        return FALSE;
    }

    BOOL ok = QueryUpdateContentLength(http.request, size);
    DWORD errorCode = ok ? ERROR_SUCCESS : GetLastError();
    CloseUpdateHttpRequest(&http);
    if (CancelUpdateTaskIfRequested(task)) return FALSE;
    if (!ok) {
        SetUpdateTaskError(task,
            L"The update server did not report a valid file size", errorCode);
        return FALSE;
    }
    if (*size > UPDATE_MAX_BYTES) {
        SetUpdateTaskError(task,
            L"The available update is unexpectedly large", 0);
        return FALSE;
    }
    return TRUE;
}

static BOOL DownloadUpdateFile(UpdateCheckTask* task,
                               ULONGLONG expectedSize) {
    if (CancelUpdateTaskIfRequested(task)) return FALSE;

    UpdateHttpRequest http;
    DWORD status = 0;
    if (!OpenUpdateHttpRequest(L"GET", task->cacheBuster, &http, &status)) {
        DWORD errorCode = GetLastError();
        if (CancelUpdateTaskIfRequested(task)) return FALSE;
        if (status) {
            swprintf_s(task->message,
                       sizeof(task->message) / sizeof(wchar_t),
                       L"The update download returned HTTP status %lu.",
                       (unsigned long)status);
            task->kind = UPDATE_CHECK_ERROR;
        } else {
            SetUpdateTaskError(task, L"Could not download the update", errorCode);
        }
        return FALSE;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        CloseUpdateHttpRequest(&http);
        return FALSE;
    }

    ULONGLONG downloadSize = 0;
    if (QueryUpdateContentLength(http.request, &downloadSize) &&
        downloadSize != expectedSize) {
        CloseUpdateHttpRequest(&http);
        SetUpdateTaskError(task,
            L"The available update changed while it was being downloaded. Try again",
            0);
        return FALSE;
    }

    DeleteUpdateTempFile(task->stagedPath);
    HANDLE file = CreateFileW(task->stagedPath, GENERIC_WRITE, 0, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD errorCode = GetLastError();
        CloseUpdateHttpRequest(&http);
        SetUpdateTaskError(task,
            L"Could not create the staged update file", errorCode);
        return FALSE;
    }

    BOOL ok = TRUE;
    ULONGLONG totalWritten = 0;
    DWORD publishedPercent = 0;
    PublishUpdateProgress(task, publishedPercent);
    BYTE buffer[64 * 1024];
    while (ok) {
        if (CancelUpdateTaskIfRequested(task)) {
            ok = FALSE;
            break;
        }

        DWORD bytesRead = 0;
        if (!WinHttpReadData(http.request, buffer, sizeof(buffer), &bytesRead)) {
            DWORD errorCode = GetLastError();
            if (!CancelUpdateTaskIfRequested(task)) {
                SetUpdateTaskError(task,
                    L"The update download was interrupted", errorCode);
            }
            ok = FALSE;
            break;
        }
        if (CancelUpdateTaskIfRequested(task)) {
            ok = FALSE;
            break;
        }
        if (bytesRead == 0) break;
        if (totalWritten + bytesRead > expectedSize) {
            SetUpdateTaskError(task,
                L"The downloaded update has an invalid size", 0);
            ok = FALSE;
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(file, buffer, bytesRead, &bytesWritten, NULL)) {
            SetUpdateTaskError(task,
                L"Could not write the staged update", GetLastError());
            ok = FALSE;
            break;
        }
        if (bytesWritten != bytesRead) {
            SetUpdateTaskError(task,
                L"Could not write the staged update", ERROR_WRITE_FAULT);
            ok = FALSE;
            break;
        }
        totalWritten += bytesWritten;

        DWORD percent = CalculateUpdateProgressPercent(totalWritten, expectedSize);
        if (percent != publishedPercent) {
            PublishUpdateProgress(task, percent);
            publishedPercent = percent;
        }
    }

    if (ok && CancelUpdateTaskIfRequested(task)) ok = FALSE;
    if (ok && totalWritten != expectedSize) {
        SetUpdateTaskError(task, L"The downloaded update is incomplete", 0);
        ok = FALSE;
    }
    if (ok && !FlushFileBuffers(file)) {
        SetUpdateTaskError(task,
            L"Could not finish writing the staged update", GetLastError());
        ok = FALSE;
    }
    CloseHandle(file);
    CloseUpdateHttpRequest(&http);

    if (ok && CancelUpdateTaskIfRequested(task)) ok = FALSE;
    DWORD binaryType = 0;
    if (ok && (!GetBinaryTypeW(task->stagedPath, &binaryType) ||
               binaryType != SCS_64BIT_BINARY)) {
        SetUpdateTaskError(task,
            L"The downloaded file is not a valid 64-bit application", 0);
        ok = FALSE;
    }
    if (!ok) DeleteUpdateTempFile(task->stagedPath);
    return ok;
}

static void DiscardUpdateTask(UpdateCheckTask* task) {
    if (!task) return;
    if (task->stagedPath[0]) DeleteUpdateTempFile(task->stagedPath);
    free(task);
}

static void PublishUpdateTask(UpdateCheckTask* task) {
    CancelUpdateTaskIfRequested(task);
    InterlockedExchange(&g_updateCheckPending, FALSE);
    InterlockedExchange(&g_updateCheckAutomatic, FALSE);
    if (!task || !IsWindow(task->targetWindow)) {
        DiscardUpdateTask(task);
        return;
    }

    UpdateCheckTask* previous = (UpdateCheckTask*)InterlockedExchangePointer(
        (PVOID volatile*)&g_updatePostedResult, task);
    DiscardUpdateTask(previous);
    if (!PostMessageW(task->targetWindow, WM_APP_UPDATE_RESULT, 0, 0)) {
        UpdateCheckTask* unclaimed = (UpdateCheckTask*)InterlockedExchangePointer(
            (PVOID volatile*)&g_updatePostedResult, NULL);
        DiscardUpdateTask(unclaimed);
    }
}

static DWORD WINAPI UpdateCheckThread(LPVOID parameter) {
    UpdateCheckTask* task = (UpdateCheckTask*)parameter;
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    DWORD pathLength = GetModuleFileNameW(NULL, task->targetPath,
                                         sizeof(task->targetPath) /
                                         sizeof(wchar_t));
    if (pathLength == 0 ||
        pathLength >= sizeof(task->targetPath) / sizeof(wchar_t)) {
        SetUpdateTaskError(task,
            L"Could not determine the running executable path", GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (!GetExecutableVersion(task->targetPath, &task->runningVersion)) {
        SetUpdateTaskError(task,
            L"Could not read the running application version", GetLastError());
        PublishUpdateTask(task);
        return 0;
    }

    ULONGLONG remoteSize = 0;
    if (!QueryRemoteUpdateSize(task, &remoteSize)) {
        PublishUpdateTask(task);
        return 0;
    }
    if (!BuildUpdateTempPath(task->stagedPath, L"download",
                             GetCurrentProcessId())) {
        SetUpdateTaskError(task,
            L"Could not create the temporary update path", GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    if (!DownloadUpdateFile(task, remoteSize)) {
        PublishUpdateTask(task);
        return 0;
    }
    if (!GetExecutableVersion(task->stagedPath, &task->availableVersion)) {
        SetUpdateTaskError(task,
            L"The downloaded application does not contain valid version information",
            GetLastError());
        PublishUpdateTask(task);
        return 0;
    }
    if (CancelUpdateTaskIfRequested(task)) {
        PublishUpdateTask(task);
        return 0;
    }

    int comparison = CompareExecutableVersions(&task->availableVersion,
                                               &task->runningVersion);
    task->kind = comparison > 0 ? UPDATE_CHECK_NEWER
               : comparison < 0 ? UPDATE_CHECK_OLDER
                                : UPDATE_CHECK_SAME;
    PublishUpdateTask(task);
    return 0;
}

static BOOL ParseUpdateProcessId(LPCWSTR text, DWORD* processId) {
    if (!text || !processId || !*text) return FALSE;
    wchar_t* end = NULL;
    unsigned long value = wcstoul(text, &end, 10);
    if (!end || *end != L'\0' || value == 0) return FALSE;
    *processId = (DWORD)value;
    return TRUE;
}

static BOOL ValidateUpdateTempFilePair(LPCWSTR helperPath,
                                       LPCWSTR stagedPath,
                                       DWORD processId) {
    if (!helperPath || !stagedPath || !*helperPath || !*stagedPath) return FALSE;

    wchar_t expectedHelperName[96], expectedStagedName[96];
    int helperNameLength = swprintf_s(expectedHelperName,
        sizeof(expectedHelperName) / sizeof(wchar_t),
        L"ChromeDevLauncher-updater-%lu.exe", (unsigned long)processId);
    int stagedNameLength = swprintf_s(expectedStagedName,
        sizeof(expectedStagedName) / sizeof(wchar_t),
        L"ChromeDevLauncher-download-%lu.exe", (unsigned long)processId);
    if (helperNameLength <= 0 || stagedNameLength <= 0 ||
        _wcsicmp(PathFindFileNameW(helperPath), expectedHelperName) != 0 ||
        _wcsicmp(PathFindFileNameW(stagedPath), expectedStagedName) != 0) {
        return FALSE;
    }

    wchar_t helperDirectory[MAX_PATH], stagedDirectory[MAX_PATH];
    if (wcscpy_s(helperDirectory, MAX_PATH, helperPath) != 0 ||
        wcscpy_s(stagedDirectory, MAX_PATH, stagedPath) != 0 ||
        !PathRemoveFileSpecW(helperDirectory) ||
        !PathRemoveFileSpecW(stagedDirectory)) {
        return FALSE;
    }
    return _wcsicmp(helperDirectory, stagedDirectory) == 0;
}

static HANDLE DuplicateUpdateLaunchToken(HANDLE process) {
    HANDLE processToken = NULL;
    HANDLE launchToken = NULL;
    if (!process ||
        !OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE,
                          &processToken)) {
        return NULL;
    }
    DuplicateTokenEx(processToken, MAXIMUM_ALLOWED, NULL,
                     SecurityImpersonation, TokenPrimary, &launchToken);
    CloseHandle(processToken);
    return launchToken;
}

static BOOL LaunchUpdateTarget(LPCWSTR targetPath, LPCWSTR stagedPath,
                               LPCWSTR helperPath, DWORD helperProcessId,
                               DWORD oldProcessId, HANDLE launchToken,
                               BOOL successfulUpdate, BOOL reopenSettings) {
    wchar_t commandLine[MAX_PATH * 3 + 256];
    LPCWSTR finishAction = successfulUpdate
        ? L"--finish-update"
        : L"--finish-update-cleanup";
    int commandLength = swprintf_s(commandLine,
        sizeof(commandLine) / sizeof(wchar_t),
        L"\"%s\" %s %lu %lu \"%s\" \"%s\"%s", targetPath, finishAction,
        (unsigned long)helperProcessId, (unsigned long)oldProcessId,
        stagedPath, helperPath,
        successfulUpdate && reopenSettings ? L" --reopen-settings-after-update" : L"");
    if (commandLength <= 0 ||
        commandLength >= (int)(sizeof(commandLine) / sizeof(wchar_t))) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    STARTUPINFOW startupInfo = {0};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {0};
    BOOL launched = FALSE;
    if (launchToken) {
        wchar_t tokenCommandLine[MAX_PATH * 3 + 256];
        wcscpy_s(tokenCommandLine,
                 sizeof(tokenCommandLine) / sizeof(wchar_t), commandLine);
        LPVOID environment = NULL;
        BOOL hasEnvironment = CreateEnvironmentBlock(&environment, launchToken,
                                                     FALSE);
        launched = CreateProcessWithTokenW(launchToken, 0, targetPath,
                                           tokenCommandLine,
                                           hasEnvironment
                                               ? CREATE_UNICODE_ENVIRONMENT : 0,
                                           environment, NULL,
                                           &startupInfo, &processInfo);
        if (environment) DestroyEnvironmentBlock(environment);
    }
    if (!launched) {
        ZeroMemory(&processInfo, sizeof(processInfo));
        launched = CreateProcessW(targetPath, commandLine, NULL, NULL, FALSE,
                                  0, NULL, NULL, &startupInfo, &processInfo);
    }
    if (launched) {
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    return launched;
}

static int RestartAfterUpdateFailure(LPCWSTR targetPath, LPCWSTR stagedPath,
                                     LPCWSTR helperPath, DWORD oldProcessId,
                                     HANDLE launchToken, LPCWSTR message) {
    MessageBoxW(NULL, message, APP_NAME L" Update", MB_OK | MB_ICONERROR);
    LaunchUpdateTarget(targetPath, stagedPath, helperPath,
                       GetCurrentProcessId(), oldProcessId, launchToken, FALSE, FALSE);
    if (launchToken) CloseHandle(launchToken);
    DeleteUpdateTempFile(stagedPath);
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
    MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 1;
}

static int RunUpdateApplyHelper(DWORD oldProcessId, LPCWSTR readyEventName,
                                LPCWSTR targetPath, LPCWSTR stagedPath,
                                BOOL reopenSettings) {
    wchar_t expectedEventPrefix[96];
    int prefixLength = swprintf_s(expectedEventPrefix,
        sizeof(expectedEventPrefix) / sizeof(wchar_t),
        L"Local\\ChromeDevLauncher_UpdateReady_%lu_",
        (unsigned long)oldProcessId);
    if (prefixLength <= 0 || !readyEventName ||
        _wcsnicmp(readyEventName, expectedEventPrefix,
                  (size_t)prefixLength) != 0) {
        return ERROR_INVALID_DATA;
    }

    HANDLE readyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEventName);
    if (!readyEvent) return (int)GetLastError();

    HANDLE oldProcess = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, oldProcessId);
    if (!oldProcess) {
        DWORD errorCode = GetLastError();
        CloseHandle(readyEvent);
        return (int)errorCode;
    }

    wchar_t oldProcessPath[MAX_PATH];
    DWORD oldProcessPathLength = sizeof(oldProcessPath) / sizeof(wchar_t);
    if (!QueryFullProcessImageNameW(oldProcess, 0, oldProcessPath,
                                    &oldProcessPathLength)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return (int)errorCode;
    }
    if (_wcsicmp(oldProcessPath, targetPath) != 0) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_INVALID_DATA;
    }

    wchar_t helperPath[MAX_PATH];
    DWORD helperPathLength = GetModuleFileNameW(
        NULL, helperPath, sizeof(helperPath) / sizeof(wchar_t));
    DWORD binaryType = 0;
    if (helperPathLength == 0 || helperPathLength >= MAX_PATH ||
        !ValidateUpdateTempFilePair(helperPath, stagedPath, oldProcessId)) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_INVALID_DATA;
    }
    if (!GetBinaryTypeW(stagedPath, &binaryType)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return (int)errorCode;
    }
    if (binaryType != SCS_64BIT_BINARY) {
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        return ERROR_BAD_EXE_FORMAT;
    }

    // Keep the original process token so the restarted app stays in the
    // same interactive user session after the elevated replacement helper.
    HANDLE launchToken = DuplicateUpdateLaunchToken(oldProcess);

    // Signal only after all paths and the process identity have been checked.
    if (!SetEvent(readyEvent)) {
        DWORD errorCode = GetLastError();
        CloseHandle(oldProcess);
        CloseHandle(readyEvent);
        if (launchToken) CloseHandle(launchToken);
        return (int)errorCode;
    }
    CloseHandle(readyEvent);

    DWORD waitResult = WaitForSingleObject(oldProcess, UPDATE_HELPER_WAIT_MS);
    CloseHandle(oldProcess);
    if (waitResult != WAIT_OBJECT_0) {
        if (launchToken) CloseHandle(launchToken);
        DeleteUpdateTempFile(stagedPath);
        SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
        MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        MessageBoxW(NULL, L"The running application did not close in time.",
                    APP_NAME L" Update", MB_OK | MB_ICONERROR);
        return ERROR_TIMEOUT;
    }

    wchar_t replacementPath[MAX_PATH], backupPath[MAX_PATH];
    DWORD helperProcessId = GetCurrentProcessId();
    int replacementLength = swprintf_s(replacementPath,
        sizeof(replacementPath) / sizeof(wchar_t), L"%s.new.%lu.exe",
        targetPath, (unsigned long)helperProcessId);
    int backupLength = swprintf_s(backupPath,
        sizeof(backupPath) / sizeof(wchar_t), L"%s.backup.%lu.exe",
        targetPath, (unsigned long)helperProcessId);
    if (replacementLength <= 0 || replacementLength >= MAX_PATH ||
        backupLength <= 0 || backupLength >= MAX_PATH) {
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The update paths were too long. The previous version will restart.");
    }

    SetFileAttributesW(replacementPath, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(replacementPath);
    SetFileAttributesW(backupPath, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(backupPath);
    if (!CopyFileW(stagedPath, replacementPath, FALSE)) {
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The update could not be prepared. The previous version will restart.");
    }

    DWORD targetAttributes = GetFileAttributesW(targetPath);
    BOOL clearedReadOnly = FALSE;
    if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
        (targetAttributes & FILE_ATTRIBUTE_READONLY)) {
        clearedReadOnly = SetFileAttributesW(
            targetPath, targetAttributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!ReplaceFileW(targetPath, replacementPath, backupPath,
                      REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
        if (clearedReadOnly) SetFileAttributesW(targetPath, targetAttributes);
        DeleteFileW(replacementPath);
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The executable could not be replaced. The previous version will restart.");
    }

    if (!LaunchUpdateTarget(targetPath, stagedPath, helperPath,
                            helperProcessId, oldProcessId, launchToken, TRUE,
                            reopenSettings)) {
        DeleteFileW(targetPath);
        if (!MoveFileExW(backupPath, targetPath,
                         MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH)) {
            if (launchToken) CloseHandle(launchToken);
            DeleteUpdateTempFile(stagedPath);
            SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
            MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            MessageBoxW(NULL,
                L"The updated application could not start and the previous "
                L"executable could not be restored. A backup remains beside "
                L"the application.",
                APP_NAME L" Update", MB_OK | MB_ICONERROR);
            return 1;
        }
        return RestartAfterUpdateFailure(targetPath, stagedPath, helperPath,
            oldProcessId, launchToken,
            L"The updated application could not start. The previous version was restored.");
    }
    if (launchToken) CloseHandle(launchToken);

    if (!DeleteFileW(backupPath)) {
        MoveFileExW(backupPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    DeleteUpdateTempFile(stagedPath);
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);
    MoveFileExW(helperPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

static BOOL FinishUpdateCleanup(DWORD helperProcessId, DWORD oldProcessId,
                                LPCWSTR stagedPath, LPCWSTR helperPath) {
    wchar_t targetPath[MAX_PATH], expectedStagedPath[MAX_PATH];
    wchar_t expectedHelperPath[MAX_PATH];
    DWORD targetLength = GetModuleFileNameW(
        NULL, targetPath, sizeof(targetPath) / sizeof(wchar_t));
    if (targetLength == 0 || targetLength >= MAX_PATH ||
        !BuildUpdateTempPath(expectedStagedPath, L"download", oldProcessId) ||
        !BuildUpdateTempPath(expectedHelperPath, L"updater", oldProcessId) ||
        _wcsicmp(stagedPath, expectedStagedPath) != 0 ||
        _wcsicmp(helperPath, expectedHelperPath) != 0) {
        return FALSE;
    }

    HANDLE helperProcess = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, helperProcessId);
    if (helperProcess) {
        wchar_t runningHelperPath[MAX_PATH];
        DWORD runningHelperPathLength = MAX_PATH;
        if (QueryFullProcessImageNameW(helperProcess, 0, runningHelperPath,
                                      &runningHelperPathLength) &&
            _wcsicmp(runningHelperPath, helperPath) == 0) {
            WaitForSingleObject(helperProcess, UPDATE_HELPER_WAIT_MS);
        }
        CloseHandle(helperProcess);
    }
    for (int attempt = 0;
         attempt < 20 && !DeleteUpdateTempFile(stagedPath); ++attempt) {
        Sleep(100);
    }
    for (int attempt = 0;
         attempt < 20 && !DeleteUpdateTempFile(helperPath); ++attempt) {
        Sleep(100);
    }
    return TRUE;
}

// Temporary updater processes stop here. Finish modes clean up the handoff
// files and then continue normal startup; updateCompleted is true only after
// a successful executable replacement.
static int HandleUpdateCommandLine(BOOL* handled, BOOL* updateCompleted,
                                    BOOL* reopenSettings) {
    if (handled) *handled = FALSE;
    if (updateCompleted) *updateCompleted = FALSE;
    if (reopenSettings) *reopenSettings = FALSE;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) return 0;

    // This option is valid only as part of an updater handoff, never alone.
    BOOL reopenRequested = argumentCount == 7 &&
        wcscmp(arguments[6], L"--reopen-settings-after-update") == 0;
    BOOL validHandoffArguments = argumentCount == 6 || reopenRequested;
    int result = 0;
    if (validHandoffArguments && wcscmp(arguments[1], L"--apply-update") == 0) {
        DWORD oldProcessId = 0;
        if (handled) *handled = TRUE;
        if (!ParseUpdateProcessId(arguments[2], &oldProcessId)) {
            result = ERROR_INVALID_PARAMETER;
        } else {
            result = RunUpdateApplyHelper(oldProcessId, arguments[3],
                                          arguments[4], arguments[5],
                                          reopenRequested);
        }
    } else if (validHandoffArguments &&
               (wcscmp(arguments[1], L"--finish-update") == 0 ||
                wcscmp(arguments[1], L"--finish-update-cleanup") == 0)) {
        DWORD helperProcessId = 0, oldProcessId = 0;
        if (ParseUpdateProcessId(arguments[2], &helperProcessId) &&
            ParseUpdateProcessId(arguments[3], &oldProcessId)) {
            BOOL recognizedHandoff = FinishUpdateCleanup(
                helperProcessId, oldProcessId, arguments[4], arguments[5]);
            if (recognizedHandoff && updateCompleted &&
                wcscmp(arguments[1], L"--finish-update") == 0) {
                *updateCompleted = TRUE;
                if (reopenSettings) *reopenSettings = reopenRequested;
            }
        }
    }
    LocalFree(arguments);
    return result;
}

// ============================================================================
// Push functions (C -> JS)
// ============================================================================

static void webview_push_init_config(void) {
    wchar_t wPath[MAX_PATH * 2];
    json_escape_wstring(g_config.chromePath, wPath, MAX_PATH * 2);
    wchar_t wAddr[128];
    json_escape_wstring(g_config.connectAddress, wAddr, 128);
    wchar_t wUpdateCompletedVersion[64];
    json_escape_wstring(
        g_updateConfirmationPending ? APP_VERSION_WSTRING : L"",
        wUpdateCompletedVersion, 64);
    wchar_t script[4096];
    swprintf(script, 4096,
        L"window.onInit({\"view\":\"config\",\"config\":{"
        L"\"chromePath\":\"%s\",\"debugPort\":%d,"
        L"\"connectAddress\":\"%s\",\"statusCheckInterval\":%d,"
        L"\"autoCheckForUpdates\":%s,\"updateCheckPending\":%s,"
        L"\"updatePromptPending\":%s},"
        L"\"updateCompletedVersion\":\"%s\"})",
        wPath, g_config.debugPort, wAddr, g_config.statusCheckInterval,
        g_config.autoCheckForUpdates ? L"true" : L"false",
        (InterlockedCompareExchange(&g_updateCheckPending,
                                    FALSE, FALSE) == TRUE ||
         InterlockedCompareExchangePointer(
             (PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL)
            ? L"true" : L"false",
        g_updateNoticeTask ? L"true" : L"false",
        wUpdateCompletedVersion);
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

static void webview_send_update_result_with_versions(
    LPCWSTR status, LPCWSTR title, LPCWSTR message,
    LPCWSTR currentVersion, LPCWSTR remoteVersion, BOOL automatic) {
    if (!g_webviewView || !status || !title || !message ||
        !currentVersion || !remoteVersion) return;

    wchar_t escapedStatus[64], escapedTitle[256], escapedMessage[1024];
    wchar_t escapedCurrentVersion[64], escapedRemoteVersion[64];
    json_escape_wstring(status, escapedStatus,
                        sizeof(escapedStatus) / sizeof(wchar_t));
    json_escape_wstring(title, escapedTitle,
                        sizeof(escapedTitle) / sizeof(wchar_t));
    json_escape_wstring(message, escapedMessage,
                        sizeof(escapedMessage) / sizeof(wchar_t));
    json_escape_wstring(currentVersion, escapedCurrentVersion,
                        sizeof(escapedCurrentVersion) / sizeof(wchar_t));
    json_escape_wstring(remoteVersion, escapedRemoteVersion,
                        sizeof(escapedRemoteVersion) / sizeof(wchar_t));

    wchar_t script[1792];
    int written = swprintf_s(script, sizeof(script) / sizeof(wchar_t),
        L"window.onUpdateResult({\"status\":\"%s\",\"title\":\"%s\","
        L"\"message\":\"%s\",\"currentVersion\":\"%s\","
        L"\"remoteVersion\":\"%s\",\"automatic\":%s})",
        escapedStatus, escapedTitle, escapedMessage,
        escapedCurrentVersion, escapedRemoteVersion,
        automatic ? L"true" : L"false");
    if (written > 0) webview_execute_script(script);
}

static void webview_send_update_result(LPCWSTR status, LPCWSTR title,
                                       LPCWSTR message) {
    webview_send_update_result_with_versions(
        status, title, message, L"", L"", FALSE);
}

static void webview_send_update_progress(DWORD percent) {
    wchar_t script[160];
    int written = swprintf_s(script, sizeof(script) / sizeof(wchar_t),
        L"window.onUpdateProgress({\"percent\":%lu})",
        (unsigned long)percent);
    if (written > 0) webview_execute_script(script);
}

static void webview_send_current_update_progress(void) {
    LONG percent = InterlockedCompareExchange(&g_updateProgressPercent, 0, 0);
    if (g_configViewReady && percent >= 0 &&
        InterlockedCompareExchange(&g_updateCheckPending, FALSE, FALSE) == TRUE) {
        webview_send_update_progress((DWORD)percent);
    }
}

static void DiscardPendingUpdateNotice(void) {
    UpdateCheckTask* task = g_updateNoticeTask;
    g_updateNoticeTask = NULL;
    DiscardUpdateTask(task);
}

static void StartUpdateCheck(BOOL automatic) {
    HWND targetWindow = g_hwnd;
    if (!targetWindow) return;
    if (automatic && (g_updateNoticeTask || g_updateReadyTask)) return;
    if (InterlockedCompareExchangePointer(
            (PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL) {
        if (!automatic) {
            webview_send_update_result(L"error", L"Update check in progress",
                L"Another update check is still finishing. Try again shortly.");
        }
        return;
    }
    if (InterlockedCompareExchange(&g_updateCheckPending, TRUE, FALSE) != FALSE) {
        if (!automatic) {
            webview_send_update_result(L"error", L"Update check in progress",
                L"Another update check is still finishing. Try again shortly.");
        }
        return;
    }
    InterlockedExchange(&g_updateCheckAutomatic, automatic ? TRUE : FALSE);

    DiscardPendingUpdateNotice();
    DiscardPreparedUpdate();

    if (!g_updateCancelEvent) {
        g_updateCancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_updateCancelEvent) {
            DWORD errorCode = GetLastError();
            InterlockedExchange(&g_updateCheckPending, FALSE);
            InterlockedExchange(&g_updateCheckAutomatic, FALSE);
            wchar_t message[256];
            swprintf_s(message, sizeof(message) / sizeof(wchar_t),
                L"Could not initialize update cancellation (Windows error %lu).",
                (unsigned long)errorCode);
            webview_send_update_result_with_versions(
                L"error", L"Update failed", message, L"", L"", automatic);
            return;
        }
    }
    ResetEvent(g_updateCancelEvent);
    InterlockedExchange(&g_updateProgressPercent, -1);
    InterlockedExchange(&g_updateProgressPosted, FALSE);

    UpdateCheckTask* task = (UpdateCheckTask*)calloc(1, sizeof(UpdateCheckTask));
    if (!task) {
        InterlockedExchange(&g_updateCheckPending, FALSE);
        InterlockedExchange(&g_updateCheckAutomatic, FALSE);
        webview_send_update_result_with_versions(
            L"error", L"Update failed",
            L"There was not enough memory to check for updates.",
            L"", L"", automatic);
        return;
    }
    task->targetWindow = targetWindow;
    task->automatic = automatic;
    LONG sequence = InterlockedIncrement(&g_updateRequestSequence);
    task->cacheBuster =
        ((GetTickCount64() ^ GetCurrentProcessId()) << 32) | (DWORD)sequence;
    if (task->cacheBuster == 0) task->cacheBuster = 1;

    HANDLE thread = CreateThread(NULL, 0, UpdateCheckThread, task, 0, NULL);
    if (!thread) {
        DWORD errorCode = GetLastError();
        free(task);
        InterlockedExchange(&g_updateCheckPending, FALSE);
        InterlockedExchange(&g_updateCheckAutomatic, FALSE);
        wchar_t message[256];
        swprintf_s(message, sizeof(message) / sizeof(wchar_t),
                   L"Could not start the update check (Windows error %lu).",
                   (unsigned long)errorCode);
        webview_send_update_result_with_versions(
            L"error", L"Update failed", message, L"", L"", automatic);
        return;
    }
    CloseHandle(thread);
}

static BOOL IsIgnoredUpdateVersion(const ExecutableVersion* version) {
    wchar_t formatted[32];
    if (!version || !g_ignoredUpdateVersion[0]) return FALSE;
    FormatExecutableVersion(version, formatted,
                            sizeof(formatted) / sizeof(wchar_t));
    return wcscmp(formatted, g_ignoredUpdateVersion) == 0;
}

static void SaveIgnoredUpdateVersion(LPCWSTR version) {
    HKEY key;
    DWORD disposition;
    if (!version) return;
    wcsncpy_s(g_ignoredUpdateVersion,
              sizeof(g_ignoredUpdateVersion) / sizeof(wchar_t), version,
              _TRUNCATE);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key,
                        &disposition) == ERROR_SUCCESS) {
        RegSetValueExW(key, REG_VALUE_IGNORED_UPDATE_VERSION, 0, REG_SZ,
                       (const BYTE*)g_ignoredUpdateVersion,
                       (DWORD)((wcslen(g_ignoredUpdateVersion) + 1) *
                               sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void PresentPendingUpdateNotice(void) {
    if (!g_configViewReady || !g_webviewView || !g_updateNoticeTask) return;

    UpdateCheckTask* task = g_updateNoticeTask;
    g_updateNoticeTask = NULL;
    LPCWSTR status = NULL;
    LPCWSTR title = NULL;
    LPCWSTR message = NULL;
    wchar_t currentVersion[32] = L"";
    wchar_t remoteVersion[32] = L"";
    BOOL installable = FALSE;

    if (task->kind == UPDATE_CHECK_CANCELLED) {
        status = L"cancelled";
        title = L"";
        message = L"";
    } else if (task->kind == UPDATE_CHECK_ERROR) {
        status = L"error";
        title = L"Update failed";
        message = task->message;
    } else {
        FormatExecutableVersionForDisplay(
            &task->runningVersion, currentVersion,
            sizeof(currentVersion) / sizeof(wchar_t));
        FormatExecutableVersionForDisplay(
            &task->availableVersion, remoteVersion,
            sizeof(remoteVersion) / sizeof(wchar_t));
        if (task->kind == UPDATE_CHECK_NEWER) {
            status = L"newer";
            title = L"Update available";
            message = L"A newer version is ready to install.";
            installable = TRUE;
        } else if (task->kind == UPDATE_CHECK_SAME) {
            status = L"same";
            title = L"You're up to date";
            message = L"The remote build matches your current version. "
                      L"You can force a reinstall if needed.";
            installable = !task->automatic;
        } else if (task->kind == UPDATE_CHECK_OLDER) {
            status = L"older";
            title = L"No update available";
            message = L"The remote build is older than your current version.";
        }
    }

    if (!status) {
        DiscardUpdateTask(task);
        return;
    }
    if (installable) {
        DiscardPreparedUpdate();
        g_updateReadyTask = task;
    }
    webview_send_update_result_with_versions(
        status, title, message, currentVersion, remoteVersion, task->automatic);
    if (!installable) DiscardUpdateTask(task);
}

static void QueueUpdateNotice(UpdateCheckTask* task) {
    DiscardPendingUpdateNotice();
    g_updateNoticeTask = task;
    PresentPendingUpdateNotice();
}

static void HandleCompletedUpdateCheck(UpdateCheckTask* task) {
    if (!task) return;
    InterlockedExchange(&g_updateProgressPosted, FALSE);
    InterlockedExchange(&g_updateProgressPercent, -1);

    if (task->automatic && !g_config.autoCheckForUpdates) {
        DiscardUpdateTask(task);
        return;
    }
    if (task->automatic && task->kind == UPDATE_CHECK_NEWER &&
        IsIgnoredUpdateVersion(&task->availableVersion)) {
        task->kind = UPDATE_CHECK_CANCELLED;
        if (g_webviewHwnd) QueueUpdateNotice(task);
        else DiscardUpdateTask(task);
        return;
    }
    if (task->automatic && task->kind == UPDATE_CHECK_NEWER) {
        BOOL configAlreadyOpen = (g_webviewHwnd != NULL);
        QueueUpdateNotice(task);
        if (!configAlreadyOpen) {
            ShowConfigDialog(g_hwnd);
            if (!g_webviewHwnd) DiscardPendingUpdateNotice();
        }
        return;
    }
    if (g_webviewHwnd) QueueUpdateNotice(task);
    else DiscardUpdateTask(task);
}

static void IgnorePreparedUpdateVersion(const char* requestedVersion) {
    UpdateCheckTask* task = g_updateReadyTask;
    wchar_t preparedDisplayVersion[32];
    wchar_t preparedStorageVersion[32];
    wchar_t requestedVersionW[32] = L"";
    if (!task || !task->automatic || task->kind != UPDATE_CHECK_NEWER ||
        !requestedVersion ||
        !MultiByteToWideChar(CP_UTF8, 0, requestedVersion, -1,
                             requestedVersionW,
                             sizeof(requestedVersionW) / sizeof(wchar_t))) {
        webview_send_update_result(L"error", L"Update unavailable",
            L"The update version could not be ignored. Check for updates again.");
        return;
    }
    FormatExecutableVersionForDisplay(
        &task->availableVersion, preparedDisplayVersion,
        sizeof(preparedDisplayVersion) / sizeof(wchar_t));
    FormatExecutableVersion(
        &task->availableVersion, preparedStorageVersion,
        sizeof(preparedStorageVersion) / sizeof(wchar_t));
    if (wcscmp(preparedDisplayVersion, requestedVersionW) != 0) {
        webview_send_update_result(L"error", L"Update unavailable",
            L"The update version changed. Check for updates again.");
        return;
    }
    SaveIgnoredUpdateVersion(preparedStorageVersion);
    DiscardPreparedUpdate();
}

static void CancelUpdateCheck(void) {
    if (InterlockedCompareExchange(&g_updateCheckPending, FALSE, FALSE) == TRUE &&
        g_updateCancelEvent) {
        SetEvent(g_updateCancelEvent);
    }
}

static HANDLE CreateUpdateReadyEvent(DWORD processId, wchar_t* eventName,
                                     size_t eventNameCch) {
    if (!processId || !eventName || eventNameCch < 96) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    ULONGLONG nonce = GetTickCount64() ^
                      ((ULONGLONG)GetCurrentThreadId() << 32);
    BCryptGenRandom(NULL, (PUCHAR)&nonce, sizeof(nonce),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    int nameLength = swprintf_s(eventName, eventNameCch,
        L"Local\\ChromeDevLauncher_UpdateReady_%lu_%016llx",
        (unsigned long)processId, (unsigned long long)nonce);
    if (nameLength <= 0 || nameLength >= (int)eventNameCch) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return NULL;
    }

    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;IU)(A;;GA;;;BA)(A;;GA;;;SY)",
            SDDL_REVISION_1, &descriptor, NULL)) {
        return NULL;
    }
    SECURITY_ATTRIBUTES securityAttributes = {0};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = descriptor;

    HANDLE readyEvent = CreateEventW(&securityAttributes, TRUE, FALSE,
                                     eventName);
    DWORD errorCode = readyEvent ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!readyEvent) SetLastError(errorCode);
    return readyEvent;
}

static BOOL LaunchStagedUpdate(LPCWSTR stagedPath, LPCWSTR targetPath,
                                BOOL reopenSettings) {
    if (!stagedPath || !targetPath || !*stagedPath || !*targetPath) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD oldProcessId = GetCurrentProcessId();
    wchar_t helperPath[MAX_PATH];
    if (!BuildUpdateTempPath(helperPath, L"updater", oldProcessId)) return FALSE;
    DeleteUpdateTempFile(helperPath);
    if (!CopyFileW(targetPath, helperPath, TRUE)) return FALSE;
    SetFileAttributesW(helperPath, FILE_ATTRIBUTE_NORMAL);

    // Do not copy the running executable's download-zone marker to the
    // short-lived helper and cause a second security warning.
    wchar_t zonePath[MAX_PATH + 32];
    if (swprintf_s(zonePath, sizeof(zonePath) / sizeof(wchar_t),
                   L"%s:Zone.Identifier", helperPath) > 0) {
        DeleteFileW(zonePath);
    }

    wchar_t readyEventName[160];
    HANDLE readyEvent = CreateUpdateReadyEvent(
        oldProcessId, readyEventName,
        sizeof(readyEventName) / sizeof(wchar_t));
    if (!readyEvent) {
        DWORD errorCode = GetLastError();
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }

    wchar_t parameters[MAX_PATH * 2 + 512];
    int parameterLength = swprintf_s(parameters,
        sizeof(parameters) / sizeof(wchar_t),
        L"--apply-update %lu \"%s\" \"%s\" \"%s\"%s",
        (unsigned long)oldProcessId, readyEventName, targetPath, stagedPath,
        reopenSettings ? L" --reopen-settings-after-update" : L"");
    if (parameterLength <= 0 ||
        parameterLength >= (int)(sizeof(parameters) / sizeof(wchar_t))) {
        CloseHandle(readyEvent);
        DeleteUpdateTempFile(helperPath);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    SHELLEXECUTEINFOW executeInfo = {0};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    executeInfo.hwnd = g_webviewHwnd;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = helperPath;
    executeInfo.lpParameters = parameters;
    executeInfo.nShow = SW_HIDE;
    BOOL elevated = ShellExecuteExW(&executeInfo);
    if (!elevated || !executeInfo.hProcess) {
        DWORD errorCode = elevated ? ERROR_INVALID_HANDLE : GetLastError();
        if (!errorCode) errorCode = ERROR_ACCESS_DENIED;
        CloseHandle(readyEvent);
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }

    HANDLE waitHandles[2] = { readyEvent, executeInfo.hProcess };
    DWORD waitResult = WaitForMultipleObjects(
        2, waitHandles, FALSE, UPDATE_HELPER_READY_MS);
    DWORD errorCode = ERROR_SUCCESS;
    if (waitResult != WAIT_OBJECT_0) {
        if (waitResult == WAIT_OBJECT_0 + 1) {
            DWORD exitCode = ERROR_INSTALL_FAILURE;
            if (!GetExitCodeProcess(executeInfo.hProcess, &exitCode) ||
                exitCode == ERROR_SUCCESS || exitCode == STILL_ACTIVE) {
                exitCode = ERROR_INSTALL_FAILURE;
            }
            errorCode = exitCode;
        } else {
            errorCode = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                   : GetLastError();
            if (!errorCode) errorCode = ERROR_INSTALL_FAILURE;
        }
    }
    CloseHandle(executeInfo.hProcess);
    CloseHandle(readyEvent);

    if (waitResult != WAIT_OBJECT_0) {
        DeleteUpdateTempFile(helperPath);
        SetLastError(errorCode);
        return FALSE;
    }
    return TRUE;
}

static void DiscardPreparedUpdate(void) {
    UpdateCheckTask* task = g_updateReadyTask;
    g_updateReadyTask = NULL;
    DiscardUpdateTask(task);
}

static void InstallPreparedUpdate(BOOL reopenSettings) {
    UpdateCheckTask* task = g_updateReadyTask;
    g_updateReadyTask = NULL;
    if (!task || (task->kind != UPDATE_CHECK_NEWER &&
                  task->kind != UPDATE_CHECK_SAME)) {
        DiscardUpdateTask(task);
        webview_send_update_result(L"error", L"Update unavailable",
            L"The prepared update is no longer available. Check for updates again.");
        return;
    }

    if (LaunchStagedUpdate(task->stagedPath, task->targetPath, reopenSettings)) {
        g_updateInstallReady = TRUE;
        free(task);  // The updater process now owns the staged file.
        if (g_webviewHwnd) PostMessageW(g_webviewHwnd, WM_CLOSE, 0, 0);
        return;
    }

    DWORD errorCode = GetLastError();
    wchar_t message[384];
    LPCWSTR title = L"Update failed";
    if (errorCode == ERROR_CANCELLED) {
        title = L"Update cancelled";
        wcscpy_s(message, sizeof(message) / sizeof(wchar_t),
            L"Administrator approval was cancelled. Your current version is still running.");
    } else {
        swprintf_s(message, sizeof(message) / sizeof(wchar_t),
            L"The elevated update process could not be started (Windows error %lu).",
            (unsigned long)errorCode);
    }
    webview_send_update_result(L"error", title, message);
    DiscardUpdateTask(task);
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
    } else if (strcmp(action, "configReady") == 0) {
        if (g_webviewHwnd) {
            BOOL updateWorkAlreadyActive =
                InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE ||
                InterlockedCompareExchangePointer(
                    (PVOID volatile*)&g_updatePostedResult, NULL, NULL) != NULL ||
                g_updateNoticeTask || g_updateReadyTask;
            BOOL checkAutomatically =
                json_get_bool(msg, "checkAutomatically", FALSE);
            g_configViewReady = TRUE;
            PresentPendingUpdateNotice();
            // Progress is published only when it changes, so a dialog opened
            // during a background download needs the current value now.
            webview_send_current_update_progress();
            if (checkAutomatically && g_config.autoCheckForUpdates &&
                !g_updateConfirmationPending && !updateWorkAlreadyActive) {
                StartUpdateCheck(TRUE);
            }
        }
    } else if (strcmp(action, "checkUpdate") == 0) {
        StartUpdateCheck(json_get_bool(msg, "automatic", FALSE));
    } else if (strcmp(action, "cancelUpdateCheck") == 0) {
        CancelUpdateCheck();
    } else if (strcmp(action, "installUpdate") == 0) {
        InstallPreparedUpdate(json_get_bool(msg, "reopenSettings", FALSE));
    } else if (strcmp(action, "dismissUpdate") == 0) {
        DiscardPreparedUpdate();
    } else if (strcmp(action, "ignoreUpdateVersion") == 0) {
        char version[32] = {0};
        json_get_string(msg, "version", version, sizeof(version));
        IgnorePreparedUpdateVersion(version);
    } else if (strcmp(action, "dismissUpdateConfirmation") == 0) {
        g_updateConfirmationPending = FALSE;
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
        g_config.autoCheckForUpdates =
            json_get_bool(msg, "autoCheckForUpdates", TRUE);
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
        // Content-driven sizing must not fight a maximized (or minimized)
        // window; WM_SIZE keeps the WebView bounds in sync there.
        if (contentHeight > 0 && g_webviewHwnd &&
            !IsZoomed(g_webviewHwnd) && !IsIconic(g_webviewHwnd)) {
            RECT clientRect = {0}, windowRect = {0};
            GetClientRect(g_webviewHwnd, &clientRect);
            GetWindowRect(g_webviewHwnd, &windowRect);
            int chromeH = (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
            int newWindowH = contentHeight + chromeH;
            int windowW = windowRect.right - windowRect.left;
            UINT flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
            BOOL firstShow = !g_webviewWindowShown;
            if (firstShow) {
                KillTimer(g_webviewHwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
            }
            SetWindowPos(g_webviewHwnd, NULL, 0, 0, windowW, newWindowH, flags);
            // Reveal the window through ShowWindow rather than SWP_SHOWWINDOW so
            // the shell registers it and creates its taskbar button.
            if (firstShow) {
                ShowWindow(g_webviewHwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(g_webviewHwnd);
            }
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
            if (InterlockedCompareExchange(&g_updateCheckPending,
                                           FALSE, FALSE) == TRUE &&
                InterlockedCompareExchange(&g_updateCheckAutomatic,
                                           FALSE, FALSE) == FALSE &&
                g_updateCancelEvent) {
                SetEvent(g_updateCancelEvent);
            }
            DiscardPendingUpdateNotice();
            DiscardPreparedUpdate();
            g_webviewHwnd = NULL;
            g_webviewWindowShown = FALSE;
            g_configViewReady = FALSE;
            KillTimer(hwnd, ID_TIMER_WEBVIEW_SHOW_FALLBACK);
            if (g_updateInstallReady) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowWebViewDialog(int width, int height) {
    // If already open, bring to front (restoring it from the taskbar if the
    // user minimized it).
    if (g_webviewHwnd != NULL) {
        if (IsIconic(g_webviewHwnd)) ShowWindow(g_webviewHwnd, SW_RESTORE);
        else ShowWindow(g_webviewHwnd, SW_SHOW);
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

    // Use the standard overlapped frame so Windows renders the normal caption
    // height instead of the more compact fixed-dialog title bar. WS_EX_APPWINDOW
    // gives the unowned configuration window its own taskbar button.
    g_webviewHwnd = CreateWindowExW(WS_EX_APPWINDOW, L"ChromeDevLauncherWebViewWnd", L"Configuration",
        WS_OVERLAPPEDWINDOW,
        posX, posY, width, height,
        NULL, NULL, g_hInstance, NULL);

    if (!g_webviewHwnd) {
        return;
    }
    g_webviewWindowShown = FALSE;
    g_configViewReady = FALSE;
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
    // Fetch /json/version to get Chrome version
    HINTERNET hSession = WinHttpOpen(L"ChromeDevLauncher Status",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return FALSE;
    WinHttpSetTimeouts(hSession, 1000, 1000, 1000, 1000);

    HINTERNET hConnect = WinHttpConnect(
        hSession, g_config.connectAddress,
        (INTERNET_PORT)g_config.debugPort, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", L"/json/version", NULL,
                             WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_REFRESH)
        : NULL;

    BOOL success = FALSE;
    g_status.chromeVersion[0] = '\0';

    if (hRequest &&
        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        char buffer[1024];
        DWORD bytesRead = 0;
        if (WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200 &&
            WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead)) {
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
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
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
    if (g_updateCancelEvent) SetEvent(g_updateCancelEvent);
    DiscardUpdateTask((UpdateCheckTask*)InterlockedExchangePointer(
        (PVOID volatile*)&g_updatePostedResult, NULL));
    DiscardPendingUpdateNotice();
    DiscardPreparedUpdate();

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

        case WM_APP_UPDATE_PROGRESS:
            InterlockedExchange(&g_updateProgressPosted, FALSE);
            webview_send_current_update_progress();
            return 0;

        case WM_APP_UPDATE_RESULT: {
            UpdateCheckTask* task = (UpdateCheckTask*)InterlockedExchangePointer(
                (PVOID volatile*)&g_updatePostedResult, NULL);
            HandleCompletedUpdateCheck(task);
            return 0;
        }

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
            } else if (wParam == ID_TIMER_AUTO_UPDATE) {
                if (g_config.autoCheckForUpdates) StartUpdateCheck(TRUE);
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
            KillTimer(hwnd, ID_TIMER_AUTO_UPDATE);
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
    BOOL updateHelperHandled = FALSE;
    BOOL updateCompleted = FALSE;
    BOOL reopenSettings = FALSE;
    int updateHelperResult = HandleUpdateCommandLine(
        &updateHelperHandled, &updateCompleted, &reopenSettings);
    if (updateHelperHandled) return updateHelperResult;

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

    // Reopen only for this successful update's explicit confirmation choice.
    if (updateCompleted && reopenSettings) {
        g_updateConfirmationPending = TRUE;
        ShowConfigDialog(g_hwnd);
    }

    // Start timers
    SetTimer(g_hwnd, ID_TIMER_STATUS_CHECK, g_config.statusCheckInterval * 1000, NULL);
    SetTimer(g_hwnd, ID_TIMER_CHROME_EXIT, CHROME_EXIT_CHECK_INTERVAL, NULL);
    SetTimer(g_hwnd, ID_TIMER_AUTO_UPDATE, AUTO_UPDATE_INTERVAL_MS, NULL);
    if (g_config.autoCheckForUpdates) StartUpdateCheck(TRUE);

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
