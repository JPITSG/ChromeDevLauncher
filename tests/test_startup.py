"""Exercise production startup helpers with an in-memory registry on Linux."""

import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "ChromeDevLauncher.c").read_text()
PATH = r"C:\Program Files\Łauncher\ChromeDevLauncher.exe"
COMMAND = f'"{PATH}"'


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        harness = "\n".join(re.findall(
            r'^#define (?:REG_APPNAME|STARTUP_RUN_KEY_W|STARTUP_APPROVED_RUN_KEY_W) '
            r'(?:\\\n[^\n]+|[^\n]+)', SOURCE, re.M)) + "\n"
        harness += (ROOT / "tests/startup_stubs.c").read_text()
        for name in ("GetStartupCommand", "IsStartWithWindowsEnabled", "SetStartWithWindows"):
            match = re.search(r"^static [^\n]*\b" + name + r"\([^;{]*\{.*?^}",
                              SOURCE, re.M | re.S)
            if not match:
                raise AssertionError(f"Function not found: {name}")
            harness += "\n" + match.group()
        harness += r'''
void reset(const wchar_t* path, const wchar_t* command, int marker, int fail) {
    wcscpy(modulePath, path);
    hasRun = command != NULL;
    wcscpy(runCommand, command ? command : L"");
    approvedMarker = marker;
    failOperation = fail;
}
int enabled(void) { return IsStartWithWindowsEnabled(); }
int apply(int enable) { return SetStartWithWindows(enable); }
const wchar_t* command(void) { return hasRun ? runCommand : NULL; }
int marker(void) { return approvedMarker; }
'''
        source = Path(cls.temp.name) / "startup.c"
        library = Path(cls.temp.name) / "startup.so"
        source.write_text(harness)
        subprocess.run(["gcc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.reset.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_int]
        cls.lib.command.restype = ctypes.c_wchar_p
        cls.lib.apply.argtypes = [ctypes.c_int]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def reset(self, command=None, marker=-1, fail=0, path=PATH):
        self.lib.reset(path, command, marker, fail)

    def test_default_and_enable_disable_round_trip(self):
        self.reset()
        self.assertFalse(self.lib.enabled())
        self.assertEqual(self.lib.apply(True), 0)
        self.assertEqual(self.lib.command(), COMMAND)
        self.assertTrue(self.lib.enabled())
        self.assertEqual(self.lib.apply(False), 0)
        self.assertIsNone(self.lib.command())
        self.assertFalse(self.lib.enabled())
        self.assertEqual(self.lib.apply(False), 0)  # Missing keys are harmless.

    def test_task_manager_disabled_entry_is_reenabled(self):
        for marker in (3, 7):
            with self.subTest(marker=marker):
                self.reset(COMMAND, marker)
                self.assertFalse(self.lib.enabled())
                self.assertEqual(self.lib.apply(True), 0)
                self.assertEqual(self.lib.marker(), -1)
                self.assertTrue(self.lib.enabled())
        for marker in (-1, 2, 6):
            self.reset(COMMAND, marker)
            self.assertTrue(self.lib.enabled())

    def test_disabling_clears_approval_marker(self):
        self.reset(COMMAND, 3)
        self.assertEqual(self.lib.apply(False), 0)
        self.assertIsNone(self.lib.command())
        self.assertEqual(self.lib.marker(), -1)

    def test_only_current_executable_counts_as_enabled(self):
        self.reset(COMMAND.replace("Program Files", "program files"))
        self.assertTrue(self.lib.enabled())
        for command in ('"C:\\Other\\ChromeDevLauncher.exe"', COMMAND + ' --unexpected', PATH):
            with self.subTest(command=command):
                self.reset(command)
                self.assertFalse(self.lib.enabled())
                self.assertEqual(self.lib.command(), command)  # Reading never rewrites it.

    def test_missing_or_truncated_module_path_cannot_be_registered(self):
        for path in ("", "x" * 260):
            self.reset(path=path)
            self.assertFalse(self.lib.enabled())
            self.assertEqual(self.lib.apply(True), 161)
            self.assertIsNone(self.lib.command())
        path = "x" * 259
        self.reset(path=path)
        self.assertEqual(self.lib.apply(True), 0)
        self.assertEqual(self.lib.command(), f'"{path}"')

    def test_registry_errors_are_returned(self):
        for fail in (1, 2):
            self.reset(fail=fail)
            self.assertEqual(self.lib.apply(True), 5)
            self.assertIsNone(self.lib.command())
        self.reset(COMMAND, fail=3)
        self.assertEqual(self.lib.apply(False), 5)
        self.assertEqual(self.lib.command(), COMMAND)
        self.reset(COMMAND, marker=3, fail=4)
        self.assertEqual(self.lib.apply(True), 5)
        self.assertFalse(self.lib.enabled())


if __name__ == "__main__":
    unittest.main()
