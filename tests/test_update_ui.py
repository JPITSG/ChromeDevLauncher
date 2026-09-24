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
                            startWithWindows: new URLSearchParams(location.search).get('startup') === 'true',
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

    def test_start_with_windows_save_and_cancel(self):
        checkbox = self.page.get_by_role("checkbox", name="Start with Windows", exact=True)
        expect(checkbox).not_to_be_checked()
        expect(self.page.get_by_text(
            "Launches in the tray when you sign in to Windows.", exact=True
        )).to_be_visible()
        updates = self.page.get_by_label("Automatically check for updates", exact=True)
        self.assertLess(checkbox.bounding_box()["y"], updates.bounding_box()["y"])
        self.page.get_by_text("Start with Windows", exact=True).click()
        expect(checkbox).to_be_checked()
        self.assertIsNone(self.last_message("saveSettings"))
        self.page.get_by_role("button", name="Cancel", exact=True).click()
        self.expect_close_prompt()
        self.page.get_by_role("button", name="Keep editing", exact=True).click()
        self.assertIsNone(self.last_message("close"))
        self.assertIsNone(self.last_message("saveSettings"))
        self.page.get_by_role("button", name="Save", exact=True).click()
        self.assertTrue(self.last_message("saveSettings")["startWithWindows"])
        checkbox.focus()
        self.page.keyboard.press("Space")
        expect(checkbox).not_to_be_checked()
        self.page.get_by_role("button", name="Save", exact=True).click()
        self.assertFalse(self.last_message("saveSettings")["startWithWindows"])

    def test_start_with_windows_loads_enabled_state(self):
        self.page.goto((ROOT / "assets/dist/index.html").as_uri() + "?startup=true")
        checkbox = self.page.get_by_role("checkbox", name="Start with Windows", exact=True)
        expect(checkbox).to_be_checked()
        self.page.get_by_role("button", name="Save", exact=True).click()
        self.assertTrue(self.last_message("saveSettings")["startWithWindows"])

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

    def close_actions(self):
        return self.page.evaluate("""() => window.messages.filter(
            m => ['close', 'saveSettings'].includes(m.action))""")

    def native_close(self):
        self.page.evaluate("window.onCloseRequested()")

    def expect_close_prompt(self):
        dialog = self.page.get_by_role("alertdialog", name="Unsaved changes", exact=True)
        expect(dialog).to_be_visible()
        expect(dialog.get_by_text("Save changes before closing?", exact=True)).to_be_visible()
        expect(self.page.get_by_role("alertdialog")).to_have_count(1)
        self.assertEqual(self.close_actions(), [])
        self.assertTrue(self.page.locator("#chromePath").evaluate(
            "el => !!el.closest('[inert]')"))
        return dialog

    def reset_config(self):
        self.page.reload()
        expect(self.page.get_by_role("button", name="Save", exact=True)).to_be_visible()

    def test_unchanged_configuration_closes_without_prompt(self):
        for trigger in (lambda: self.page.get_by_role("button", name="Cancel", exact=True).click(),
                        self.native_close, lambda: self.page.keyboard.press("Escape")):
            self.reset_config()
            trigger()
            self.assertEqual(self.close_actions(), [{"action": "close"}])
            expect(self.page.get_by_role("alertdialog")).to_have_count(0)

    def test_each_setting_prompts_and_reverting_allows_close(self):
        fields = (
            ("#chromePath", r"C:\Other\chrome.exe", r"C:\Chrome\chrome.exe"),
            ("#debugPort", "9333", "9222"),
            ("#connectAddress", "127.0.0.2", "127.0.0.1"),
            ("#statusCheckInterval", "120", "60"),
            ("#start-with-windows", True, False),
            ("#autoCheckForUpdates", True, False),
        )
        for selector, changed, original in fields:
            with self.subTest(setting=selector):
                self.reset_config()
                field = self.page.locator(selector)
                if isinstance(changed, bool):
                    field.set_checked(changed)
                else:
                    field.fill(changed)
                self.native_close()
                self.expect_close_prompt()
                self.native_close()  # Repeated X cannot bypass the question.
                self.expect_close_prompt()
                self.page.get_by_role("button", name="Keep editing").click()
                expect(self.page.get_by_role("alertdialog")).to_have_count(0)
                if isinstance(original, bool):
                    expect(field).to_be_checked()
                    field.set_checked(original)
                else:
                    expect(field).to_have_value(changed)
                    field.fill(original)
                self.page.get_by_role("button", name="Cancel", exact=True).click()
                self.assertEqual(self.close_actions(), [{"action": "close"}])
                expect(self.page.get_by_role("alertdialog")).to_have_count(0)

    def test_browsing_to_a_different_chrome_path_counts_as_an_edit(self):
        self.page.evaluate("window.onBrowseResult({path: 'D:\\\\Chrome\\\\chrome.exe'})")
        self.native_close()
        self.expect_close_prompt()

    def test_prompt_keyboard_overlay_and_discard(self):
        checkbox = self.page.get_by_label("Start with Windows", exact=True)
        checkbox.check()
        original_height = self.last_message("resize")["height"]
        cancel = self.page.get_by_role("button", name="Cancel", exact=True)
        cancel.click()
        dialog = self.expect_close_prompt()
        keep = dialog.get_by_role("button", name="Keep editing")
        save = dialog.get_by_role("button", name="Save", exact=True)
        expect(keep).to_be_focused()
        self.page.keyboard.press("Tab")
        expect(dialog.get_by_role("button", name="Discard")).to_be_focused()
        self.page.keyboard.press("Tab")
        expect(save).to_be_focused()
        self.page.keyboard.press("Tab")
        expect(keep).to_be_focused()
        self.page.keyboard.press("Shift+Tab")
        expect(save).to_be_focused()
        self.assertEqual(dialog.evaluate(
            "el => getComputedStyle(el.parentElement).backgroundColor"), "rgba(0, 0, 0, 0.35)")
        self.assertEqual(self.last_message("resize")["height"], original_height)
        self.page.mouse.click(8, 8)
        self.expect_close_prompt()
        self.page.keyboard.press("Escape")
        expect(self.page.get_by_role("alertdialog")).to_have_count(0)
        expect(checkbox).to_be_checked()
        expect(cancel).to_be_focused()
        self.page.keyboard.press("Escape")
        dialog = self.expect_close_prompt()
        dialog.get_by_role("button", name="Discard").click()
        self.assertEqual(self.close_actions(), [{"action": "close"}])

    def test_prompt_save_matches_normal_save(self):
        for through_prompt in (False, True):
            with self.subTest(through_prompt=through_prompt):
                self.reset_config()
                self.page.get_by_label("Start with Windows", exact=True).check()
                self.page.get_by_label("Debug Port", exact=True).fill("9333")
                scope = self.page
                if through_prompt:
                    self.native_close()
                    scope = self.expect_close_prompt()
                scope.get_by_role("button", name="Save", exact=True).click()
                self.assertEqual(self.close_actions(), [{
                    "action": "saveSettings", "chromePath": r"C:\Chrome\chrome.exe",
                    "debugPort": 9333, "connectAddress": "127.0.0.1",
                    "statusCheckInterval": 60, "startWithWindows": True,
                    "autoCheckForUpdates": False,
                }])

    def test_invalid_prompt_save_returns_to_the_field_without_losing_edits(self):
        for selector, invalid, valid, error in (
            ("#debugPort", "70000", "9333", "Port must be between 1 and 65535"),
            ("#statusCheckInterval", "4", "120", "Interval must be at least 5 seconds"),
        ):
            with self.subTest(setting=selector):
                self.reset_config()
                field = self.page.locator(selector)
                field.fill(invalid)
                self.page.get_by_label("Start with Windows", exact=True).check()
                self.native_close()
                self.expect_close_prompt().get_by_role("button", name="Save", exact=True).click()
                expect(self.page.get_by_role("alertdialog")).to_have_count(0)
                expect(self.page.get_by_text(error, exact=True)).to_be_visible()
                expect(field).to_be_focused()
                expect(field).to_have_value(invalid)
                self.assertEqual(self.close_actions(), [])
                field.fill(valid)
                self.page.get_by_role("button", name="Save", exact=True).click()
                self.assertTrue(self.last_message("saveSettings")["startWithWindows"])
                self.assertIsNone(self.last_message("close"))

    def test_unsaved_prompt_has_priority_over_update_results(self):
        checkbox = self.page.get_by_label("Start with Windows", exact=True)
        checkbox.check()
        self.native_close()
        self.expect_close_prompt()
        self.result("newer", automatic=True)
        self.expect_close_prompt()
        self.page.get_by_role("button", name="Keep editing").click()
        update = self.page.get_by_role("alertdialog", name="Update available", exact=True)
        expect(update).to_be_visible()
        self.assertEqual(update.evaluate(
            "el => getComputedStyle(el.parentElement).backgroundColor"), "rgba(0, 0, 0, 0.35)")
        self.native_close()
        self.expect_close_prompt()
        self.page.keyboard.press("Escape")
        expect(update).to_be_visible()
        update.get_by_role("button", name="Cancel", exact=True).click()
        expect(self.page.get_by_role("alertdialog")).to_have_count(0)
        expect(checkbox).to_be_checked()
        self.assertEqual(self.close_actions(), [])

    def test_update_only_changes_do_not_prompt_to_save(self):
        self.result()
        self.page.get_by_label("Reopen settings after update", exact=True).check()
        self.page.get_by_role("alertdialog").get_by_role("button", name="Cancel").click()
        self.native_close()
        expect(self.page.get_by_role("alertdialog")).to_have_count(0)
        self.assertEqual(self.close_actions(), [{"action": "close"}])

    def test_progress_format_red_style_and_cancellation(self):
        button = self.page.get_by_role("button", name="Update", exact=True)
        button.click()
        button = self.page.get_by_role("button", name="Stop update check and download")
        expect(button).to_have_text("Checking...")
        expect(button).to_have_class(re.compile("bg-red-"))
        for percent, label in ((0, "0"), (7, "7"), (42.9, "42"), (100, "100"),
                               (150, "100"), (-5, "0")):
            self.page.evaluate("percent => window.onUpdateProgress({percent})", percent)
            expect(button).to_have_text(f"Checking ({label}%)...")
            expect(button).to_be_enabled()  # Clicking again stops the download.
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
