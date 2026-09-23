// Exercise the production result handler and existing-window reveal path.
// No window, download, configuration save, or updater process is created.
#include <assert.h>
#include <stddef.h>

typedef int BOOL;
typedef void* HWND;
typedef struct { int ignored; } ExecutableVersion;
typedef struct {
    BOOL automatic;
    int kind;
    ExecutableVersion availableVersion;
} UpdateCheckTask;
#define TRUE 1
#define FALSE 0
#define SW_RESTORE 9
#define SW_SHOW 5
enum { UPDATE_CHECK_SAME, UPDATE_CHECK_NEWER, UPDATE_CHECK_OLDER,
       UPDATE_CHECK_CANCELLED, UPDATE_CHECK_ERROR };

static HWND g_hwnd = (HWND)1, g_webviewHwnd;
static struct { BOOL autoCheckForUpdates; } g_config;
static int g_updateProgressPosted, g_updateProgressPercent;
static int minimized, opens, raises, shown, discarded, notices;
static UpdateCheckTask* pending;

static void InterlockedExchange(int* value, int next) { *value = next; }
static BOOL IsIgnoredUpdateVersion(const ExecutableVersion* version) {
    return version->ignored;
}
static void DiscardUpdateTask(UpdateCheckTask* task) { (void)task; ++discarded; }
static void QueueUpdateNotice(UpdateCheckTask* task) { pending = task; ++notices; }
static void DiscardPendingUpdateNotice(void) { pending = NULL; }
static BOOL IsIconic(HWND window) { assert(window == g_webviewHwnd); return minimized; }
static void ShowWindow(HWND window, int mode) {
    assert(window == g_webviewHwnd);
    shown = mode;
    if (mode == SW_RESTORE) minimized = FALSE;
}
static void SetForegroundWindow(HWND window) {
    assert(window == g_webviewHwnd && pending);
    ++raises;
}
static BOOL ShowConfigDialog(HWND parent) {
    assert(parent == g_hwnd && pending);
    ++opens;
    g_webviewHwnd = (HWND)2;
    return FALSE;
}

/* PRODUCTION FUNCTIONS */

static void reset(BOOL existing, BOOL iconic) {
    g_webviewHwnd = existing ? (HWND)2 : NULL;
    g_config.autoCheckForUpdates = TRUE;
    minimized = iconic;
    opens = raises = shown = discarded = notices = 0;
    pending = NULL;
}

int main(void) {
    UpdateCheckTask task = { TRUE, UPDATE_CHECK_NEWER, { FALSE } };
    reset(TRUE, TRUE);
    pending = &task;
    ShowWebViewDialog(480, 340);
    assert(raises == 1 && shown == SW_RESTORE && !minimized);

    reset(FALSE, FALSE);
    HandleCompletedUpdateCheck(&task);
    assert(opens == 1 && notices == 1 && discarded == 0);

    // An open panel must be raised without entering another configuration
    // message loop, which would reset the existing editing session.
    reset(TRUE, FALSE);
    HandleCompletedUpdateCheck(&task);
    assert(opens == 0 && raises == 1 && shown == SW_SHOW && notices == 1);

    reset(TRUE, TRUE);
    HandleCompletedUpdateCheck(&task);
    assert(opens == 0 && raises == 1 && shown == SW_RESTORE && !minimized);

    reset(TRUE, TRUE);
    g_config.autoCheckForUpdates = FALSE;
    HandleCompletedUpdateCheck(&task);
    assert(discarded == 1 && opens == 0 && raises == 0 && notices == 0);

    for (int existing = 0; existing <= 1; ++existing) {
        reset(existing, TRUE);
        task.kind = UPDATE_CHECK_NEWER;
        task.availableVersion.ignored = TRUE;
        HandleCompletedUpdateCheck(&task);
        assert(opens == 0 && raises == 0 && minimized);
        assert(existing ? task.kind == UPDATE_CHECK_CANCELLED : discarded == 1);
    }
    task.availableVersion.ignored = FALSE;

    for (int kind = UPDATE_CHECK_SAME; kind <= UPDATE_CHECK_ERROR; ++kind) {
        if (kind == UPDATE_CHECK_NEWER) continue;
        reset(FALSE, FALSE);
        task.kind = kind;
        HandleCompletedUpdateCheck(&task);
        assert(opens == 0 && raises == 0 && notices == 0 && discarded == 1);
    }
    reset(TRUE, TRUE);
    task.kind = UPDATE_CHECK_NEWER;
    task.automatic = FALSE;
    HandleCompletedUpdateCheck(&task);
    assert(opens == 0 && raises == 0 && notices == 1 && minimized);
    return 0;
}
