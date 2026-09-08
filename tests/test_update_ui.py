"""Test the built UI with a mocked WebView bridge; never install an update.

Requires Python Playwright and Chromium. Build first with make, then run:
python3 -m unittest discover -s tests -p 'test_update_ui.py' -v
CHROME_EXECUTABLE may select a local Chromium executable.
"""

import os
import re
from pathlib import Path
import unittest

from playwright.sync_api import expect, sync_playwright


ROOT = Path(__file__).resolve().parents[1]


class UpdateUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        options = {"headless": True, "args": ["--no-sandbox"]}
        if os.environ.get("CHROME_EXECUTABLE"):
            options["executable_path"] = os.environ["CHROME_EXECUTABLE"]
        cls.browser = cls.playwright.chromium.launch(**options)

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.page = self.browser.new_page(viewport={"width": 460, "height": 560})
        self.page.add_init_script("""
            window.messages = [];
            window.chrome = { webview: { postMessage: (text) => {
                const message = JSON.parse(text);
                window.messages.push(message);
                if (message.action === 'getInit') {
                    queueMicrotask(() => window.onInit({
                        view: 'config', updateCompletedVersion: '',
                        config: { chromePath: 'C:\\\\Chrome\\\\chrome.exe',
                            debugPort: 9222, connectAddress: '127.0.0.1',
                            statusCheckInterval: 60, autoCheckForUpdates: false,
                            updateCheckPending: false, updatePromptPending: false }
                    }));
                }
            } } };
        """)
        self.page.goto((ROOT / "assets/dist/index.html").as_uri())
        expect(self.page.get_by_role("button", name="Update", exact=True)).to_be_visible()

    def tearDown(self):
        self.page.close()

    def result(self, status="newer", automatic=False):
        self.page.evaluate("result => window.onUpdateResult(result)", {
            "status": status, "title": "Update available", "message": "Ready.",
            "currentVersion": "1.0.3", "remoteVersion": "1.0.4",
            "automatic": automatic,
        })

    def last_message(self, action):
        return self.page.evaluate(
            "action => window.messages.filter(m => m.action === action).at(-1)",
            action)

    def test_speed_format_red_style_and_cancellation(self):
        button = self.page.get_by_role("button", name="Update", exact=True)
        button.click()
        button = self.page.get_by_role("button", name="Stop update check and download")
        expect(button).to_have_text("Checking...")
        expect(button).to_have_class(re.compile("bg-red-"))
        for speed, label in ((100, "100"), (100.5, "101"), (12345, "12345"), (0, "0")):
            self.page.evaluate("speed => window.onUpdateProgress({kilobytesPerSecond: speed})", speed)
            expect(button).to_have_text(f"Checking ({label}kb/s)...")
            expect(button).to_be_enabled()
        button.click()
        expect(button).to_have_text("Stopping...")
        expect(button).to_be_disabled()
        self.assertEqual(self.last_message("cancelUpdateCheck"), {"action": "cancelUpdateCheck"})
        self.result("cancelled")
        button = self.page.get_by_role("button", name="Update", exact=True)
        expect(button).to_be_enabled()
        button.click()
        expect(self.page.get_by_text("Checking...", exact=True)).to_be_visible()

    def test_checkbox_defaults_resets_and_is_not_saved(self):
        self.result()
        checkbox = self.page.get_by_label("Reopen settings after update", exact=True)
        expect(checkbox).not_to_be_checked()
        checkbox.check()
        self.page.get_by_role("alertdialog").get_by_role("button", name="Cancel").click()
        self.result()
        expect(checkbox).not_to_be_checked()
        checkbox.check()
        self.result("newer", automatic=True)
        expect(checkbox).not_to_be_checked()
        checkbox.check()
        self.page.get_by_role("button", name="Ignore this version").click()
        self.result("same")
        expect(checkbox).not_to_be_checked()
        self.page.get_by_role("alertdialog").get_by_role("button", name="Cancel").click()
        self.page.get_by_role("button", name="Save", exact=True).click()
        self.assertNotIn("reopenSettings", self.last_message("saveSettings"))

    def test_checked_install_and_failure_reset(self):
        self.result()
        checkbox = self.page.get_by_label("Reopen settings after update", exact=True)
        checkbox.check()
        self.page.get_by_role("alertdialog").get_by_role("button", name="Update", exact=True).click()
        self.assertEqual(self.last_message("installUpdate"), {
            "action": "installUpdate", "reopenSettings": True})
        expect(checkbox).to_be_disabled()
        expect(self.page.get_by_role("button", name="Starting...")).to_be_disabled()
        self.result("error")  # Includes UAC cancellation errors from the host.
        expect(checkbox).to_have_count(0)
        self.page.get_by_role("button", name="OK", exact=True).click()
        self.result()
        expect(checkbox).not_to_be_checked()

    def test_unchecked_force_update(self):
        self.result("same")
        checkbox = self.page.get_by_label("Reopen settings after update", exact=True)
        expect(checkbox).not_to_be_checked()
        self.page.get_by_role("button", name="Force update", exact=True).click()
        self.assertEqual(self.last_message("installUpdate"), {
            "action": "installUpdate", "reopenSettings": False})

    def test_older_version_cannot_install(self):
        self.result("older")
        expect(self.page.get_by_label("Reopen settings after update")).to_have_count(0)
        expect(self.page.get_by_role("alertdialog").get_by_role("button", name="OK")).to_be_visible()
        self.assertIsNone(self.last_message("installUpdate"))


if __name__ == "__main__":
    unittest.main()
