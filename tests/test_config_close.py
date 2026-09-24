"""Run the production close gate and WM_CLOSE branch with harmless window doubles."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "ChromeDevLauncher.c").read_text()


class ConfigCloseTests(unittest.TestCase):
    def test_native_close_waits_for_ui_before_destroying_the_window(self):
        gate = re.search(r"^static BOOL RequestConfigClose\(void\) \{.*?^}",
                         SOURCE, re.M | re.S)
        self.assertIsNotNone(gate)
        proc = SOURCE.split("static LRESULT CALLBACK WebViewWndProc(", 1)[1]
        close = proc.split("case WM_CLOSE:", 1)[1].split("case WM_DESTROY:", 1)[0]
        harness = PREFIX + gate.group() + "\nstatic int close_window(HWND hwnd) {" + close + "}\n" + SCENARIOS
        with tempfile.TemporaryDirectory(prefix="chromedevlauncher-close-") as directory:
            path = Path(directory)
            (path / "close.c").write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "close.c"), "-o", str(path / "close")], check=True)
            subprocess.run([str(path / "close")], check=True, timeout=10)


PREFIX = r'''
#include <assert.h>
#include <stddef.h>
#include <wchar.h>
typedef int BOOL;
typedef void* HWND;
#define TRUE 1
#define FALSE 0
#define ID_TIMER_WEBVIEW_SHOW_FALLBACK 1006
static BOOL g_configViewReady, g_configCloseApproved, g_updateInstallReady;
static BOOL g_webviewWindowShown;
static int requests, timerStops, closed, released, destroyed;
static HWND window = (HWND)1;

typedef struct Interface Interface;
typedef struct {
    void (*Close)(Interface*);
    void (*Release)(Interface*);
} VTable;
struct Interface { const VTable* lpVtbl; };
static void close_interface(Interface* self) { assert(self); closed++; }
static void release_interface(Interface* self) { assert(self); released++; }
static const VTable methods = {close_interface, release_interface};
static Interface controller = {&methods}, view = {&methods}, environment = {&methods};
static Interface *g_webviewController, *g_webviewView, *g_webviewEnv;
static void webview_execute_script(const wchar_t* script) {
    assert(wcscmp(script, L"window.onCloseRequested()") == 0);
    requests++;
}
static void KillTimer(HWND hwnd, int id) {
    assert(hwnd == window && id == ID_TIMER_WEBVIEW_SHOW_FALLBACK);
    timerStops++;
}
static void DestroyWindow(HWND hwnd) {
    assert(hwnd == window);
    destroyed++;
}
static void reset(BOOL ready, BOOL hasView) {
    g_configViewReady = ready;
    g_webviewController = &controller;
    g_webviewEnv = &environment;
    g_webviewView = hasView ? &view : NULL;
    g_configCloseApproved = g_updateInstallReady = FALSE;
    g_webviewWindowShown = TRUE;
    requests = timerStops = closed = released = destroyed = 0;
}
'''

SCENARIOS = r'''
int main(void) {
    reset(TRUE, TRUE);
    for (int i = 1; i <= 3; i++) {
        assert(close_window(window) == 0);  // X, Alt+F4 or repeated close.
        assert(requests == i);
        assert(timerStops == 0 && closed == 0 && released == 0 && destroyed == 0);
        assert(g_webviewWindowShown && g_webviewController && g_webviewView && g_webviewEnv);
        assert(!g_configCloseApproved);
    }
    g_configCloseApproved = TRUE;  // UI saved, discarded or found no edits.
    assert(close_window(window) == 0);
    assert(requests == 3 && timerStops == 1 && closed == 1 && released == 3 && destroyed == 1);
    assert(!g_webviewWindowShown && !g_webviewController && !g_webviewView && !g_webviewEnv);

    // A loading or failed WebView can still be closed without getting stuck.
    for (int ready = 0; ready <= 1; ready++) {
        for (int hasView = 0; hasView <= 1; hasView++) {
            if (ready && hasView) continue;
            reset(ready, hasView);
            assert(close_window(window) == 0);
            assert(requests == 0 && destroyed == 1 && closed == 1 && released == 2 + hasView);
        }
    }

    reset(TRUE, TRUE);
    g_updateInstallReady = TRUE;  // Successful updater handoff must finish.
    assert(close_window(window) == 0);
    assert(requests == 0 && destroyed == 1);

    reset(TRUE, TRUE);
    g_configCloseApproved = TRUE;  // Application shutdown must finish synchronously.
    assert(close_window(window) == 0);
    assert(requests == 0 && destroyed == 1);
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()
