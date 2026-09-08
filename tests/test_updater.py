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
        for name in ("CalculateUpdateSpeedKbps", "ParseUpdateProcessId",
                     "LaunchUpdateTarget", "HandleUpdateCommandLine"):
            harness += "\n" + function(name)
        harness += r'''
unsigned speed(unsigned long long bytes, unsigned long long ms) {
    return CalculateUpdateSpeedKbps(bytes, ms);
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
        cls.lib.speed.argtypes = [ctypes.c_ulonglong, ctypes.c_ulonglong]
        cls.lib.speed.restype = ctypes.c_uint
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

    def test_speed_rounding_and_elapsed_time(self):
        for received, elapsed, expected in (
            (25600, 250, 100), (25728, 250, 101), (25727, 250, 100),
            (1, 1000, 0), (512, 1000, 1), (0, 250, 0),
            (1024, 0, 0), (1024, 500, 2), (1024, 2000, 1),
            (100 * 1024 * 1024, 250, 409600),
            (100 * 1024 * 1024, 2**32, 0),
        ):
            with self.subTest(received=received, elapsed=elapsed):
                self.assertEqual(self.lib.speed(received, elapsed), expected)

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
        self.assertIn("speedWindowBytes += bytesRead", download)
        self.assertIn("GetTickCount64()", download)
        self.assertIn("elapsed >= UPDATE_PROGRESS_INTERVAL_MS", download)
        self.assertIn("InterlockedCompareExchange(&g_updateProgressPosted",
                      function("PublishUpdateProgress"))

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
