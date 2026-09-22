"""Exercise production updater functions with harmless Linux API doubles.

Run: python3 -m unittest discover -s tests -p 'test_updater.py' -v
No updater, replacement executable, or Windows process is launched.
"""

import ctypes
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "ChromeDevLauncher.c").read_text()


def function(name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;{]*\{.*?^}",
                      SOURCE, re.M | re.S)
    if not match:
        raise AssertionError(f"Function not found: {name}")
    return match.group()


class UpdaterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        harness = (ROOT / "tests/updater_stubs.c").read_text()
        for name in ("CalculateUpdateProgressPercent", "ParseUpdateProcessId",
                     "LaunchUpdateTarget", "HandleUpdateCommandLine"):
            harness += "\n" + function(name)
        harness += r'''
unsigned progress(unsigned long long received, unsigned long long total) {
    return CalculateUpdateProgressPercent(received, total);
}
const wchar_t* launch(int success, int reopen, int token) {
    LaunchUpdateTarget(L"C:\\Program Files\\Launcher.exe",
        L"C:\\Temp Dir\\download.exe", L"C:\\Temp Dir\\helper.exe",
        123, 456, token ? (HANDLE)1 : NULL, success, reopen);
    return capturedCommand;
}
int route(int argc, wchar_t** argv, int recognized, int* outputs) {
    testArgc = argc; testArgv = argv; validCleanup = recognized;
    applyCalls = cleanupCalls = 0; applyReopen = -1;
    outputs[0] = outputs[1] = outputs[2] = TRUE;
    int result = HandleUpdateCommandLine(outputs, outputs + 1, outputs + 2);
    outputs[3] = applyCalls; outputs[4] = cleanupCalls;
    outputs[5] = applyReopen;
    return result;
}
'''
        source = Path(cls.temp.name) / "updater.c"
        library = Path(cls.temp.name) / "updater.so"
        source.write_text(harness)
        subprocess.run(["gcc", "-shared", "-fPIC", "-Wall", "-Werror",
                        str(source), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.progress.argtypes = [ctypes.c_ulonglong, ctypes.c_ulonglong]
        cls.lib.progress.restype = ctypes.c_uint
        cls.lib.launch.argtypes = [ctypes.c_int] * 3
        cls.lib.launch.restype = ctypes.c_wchar_p
        cls.lib.route.argtypes = [ctypes.c_int,
                                 ctypes.POINTER(ctypes.c_wchar_p), ctypes.c_int,
                                 ctypes.POINTER(ctypes.c_int)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def route(self, args, recognized=True):
        argv = (ctypes.c_wchar_p * len(args))(*args)
        outputs = (ctypes.c_int * 6)()
        result = self.lib.route(len(args), argv, recognized, outputs)
        return result, list(outputs)

    def test_progress_percent_rounds_down(self):
        maximum = 100 * 1024 * 1024
        for received, total, expected in (
            (0, 1000, 0), (9, 1000, 0), (10, 1000, 1),
            (1, 3, 33), (2, 3, 66), (999, 1000, 99),
            (1000, 1000, 100), (2000, 1000, 100), (5, 0, 0),
            (maximum - 1, maximum, 99), (maximum, maximum, 100),
        ):
            with self.subTest(received=received, total=total):
                self.assertEqual(self.lib.progress(received, total), expected)

    def test_successful_relaunch_round_trip(self):
        for reopen in (False, True):
            for token in (False, True):
                with self.subTest(reopen=reopen, token=token):
                    command = self.lib.launch(True, reopen, token)
                    args = shlex.split(command)
                    self.assertEqual(args[0], r"C:\Program Files\Launcher.exe")
                    self.assertEqual(args[4], r"C:\Temp Dir\download.exe")
                    result, outputs = self.route(args)
                    self.assertEqual(result, 0)
                    self.assertEqual(outputs[:5], [0, 1, int(reopen), 0, 1])

    def test_rollback_never_reopens(self):
        for requested in (False, True):
            command = self.lib.launch(False, requested, True)
            self.assertNotIn("--reopen-settings-after-update", command)
            _, outputs = self.route(shlex.split(command))
            self.assertEqual(outputs[:5], [0, 0, 0, 0, 1])

    def test_apply_passes_choice_to_helper(self):
        args = ["helper.exe", "--apply-update", "456", "ready-event",
                "target.exe", "staged.exe"]
        for reopen in (False, True):
            _, outputs = self.route(args + (
                ["--reopen-settings-after-update"] if reopen else []))
            self.assertEqual(outputs, [1, 0, 0, 1, 0, int(reopen)])

    def test_invalid_or_unrelated_launch_cannot_reopen(self):
        finish = shlex.split(self.lib.launch(True, True, False))
        cases = [(["app.exe"], True),
                 (["app.exe", "--reopen-settings-after-update"], True),
                 (finish, False),
                 (finish[:2] + ["0"] + finish[3:], True),
                 (finish[:-1] + ["--unknown"], True),
                 (finish + ["extra"], True)]
        for args, recognized in cases:
            with self.subTest(args=args, recognized=recognized):
                _, outputs = self.route(args, recognized)
                self.assertEqual(outputs[:3], [0, 0, 0])
        # Even a cleanup command with a stray reopen option cannot reopen.
        finish[1] = "--finish-update-cleanup"
        self.assertEqual(self.route(finish)[1][:3], [0, 0, 0])

    def test_transport_and_startup_guards(self):
        self.assertIn('json_get_bool(msg, "reopenSettings", FALSE)', SOURCE)
        self.assertIn('reopenSettings ? L" --reopen-settings-after-update"',
                      function("LaunchStagedUpdate"))
        self.assertIn("task->targetPath, reopenSettings)",
                      function("InstallPreparedUpdate"))
        self.assertIn("launchToken, FALSE, FALSE)",
                      function("RestartAfterUpdateFailure"))
        self.assertIn("launchToken, TRUE,\n                            reopenSettings)",
                      function("RunUpdateApplyHelper"))
        self.assertIn("if (updateCompleted && reopenSettings)", SOURCE)
        self.assertIn("PathGetArgsW(GetCommandLineW())", function("SelfElevate"))
        self.assertNotIn("reopenSettings", function("SaveConfigToRegistry"))
        download = function("DownloadUpdateFile")
        # 0% once the body transfer starts, then only when the percentage changes.
        self.assertIn("PublishUpdateProgress(task, publishedPercent);\n    BYTE buffer",
                      download)
        self.assertIn("CalculateUpdateProgressPercent(totalWritten, expectedSize)",
                      download)
        self.assertIn("if (percent != publishedPercent)", download)
        self.assertIn("InterlockedCompareExchange(&g_updateProgressPosted",
                      function("PublishUpdateProgress"))
        self.assertIn("g_configViewReady && percent >= 0",
                      function("webview_send_current_update_progress"))
        self.assertIn("webview_send_current_update_progress();",
                      function("MsgReceived_Invoke"))

    def test_version_metadata_is_synchronized(self):
        package = json.loads((ROOT / "assets/package.json").read_text())
        lock = json.loads((ROOT / "assets/package-lock.json").read_text())
        header = (ROOT / "version.h").read_text()
        version = package["version"]
        self.assertEqual(lock["version"], version)
        self.assertEqual(lock["packages"][""]["version"], version)
        self.assertIn(f'APP_VERSION_STRING "{version}"', header)
        self.assertIn(f'APP_FILE_VERSION_STRING "{version}.0"', header)
        self.assertIn(f'APP_VERSION_RESOURCE {version.replace(".", ",")},0', header)


if __name__ == "__main__":
    unittest.main()
