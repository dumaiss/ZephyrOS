import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ProjectTests(unittest.TestCase):
    def test_executable_stays_below_stack(self):
        report = json.loads(
            (ROOT / "build/user/konacmd/konacmd.report.json").read_text())
        end = 0x100 + report["header"]["image_size"] + report["header"]["bss_size"]
        self.assertLess(end, report["stack"]["base"])
        self.assertGreaterEqual(report["stack"]["base"], 0xC000)

    def test_local_and_remote_panes_and_fast_navigation_are_present(self):
        source = (ROOT / "src/kc_ui.c").read_text()
        self.assertIn("panels[0].source = PANEL_LOCAL", source)
        self.assertIn("panels[1].source = PANEL_REMOTE", source)
        self.assertIn("render_navigation(old_selected)", source)

    def test_ui_uses_the_tui_sdk_without_terminal_sequences(self):
        source = (ROOT / "src/kc_ui.c").read_text()
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn('#include "tui.h"', source)
        self.assertIn('#include "tui_widgets.h"', source)
        self.assertIn("LIBS=tui_widgets:tui", makefile)
        self.assertIn("kos-sdk-0.2.0.tar.gz", makefile)
        self.assertNotIn("\\033", source)
        self.assertNotIn("console_write", source)
        self.assertNotIn("getchar()", source)

    def test_focus_and_menu_navigation_defaults_are_present(self):
        source = (ROOT / "src/kc_ui.c").read_text()
        self.assertIn("if (key == TUI_KEY_F1)", source)
        self.assertIn("focus_set(FOCUS_MENU)", source)
        self.assertIn("menu_move(key == TUI_KEY_LEFT ? -1 : 1)", source)
        self.assertIn("if (key == '\\t')", source)
        self.assertIn("focus_next();", source)
        self.assertIn("render_focus_change(old_focus, old_active", source)
        self.assertIn("ui_focus == FOCUS_HELP", source)
        self.assertIn("ui_focus == FOCUS_QUIT", source)

    def test_popup_navigation_only_redraws_changed_rows(self):
        source = (ROOT / "src/kc_ui.c").read_text()
        menu = source.split("static unsigned char run_menu(void)")[1].split(
            "static unsigned char focused_action")[0]
        self.assertIn("render_popup_selection(items, old_selected, selected",
                      menu)
        self.assertIn("tui_save(box, menu_background", source)
        self.assertIn("tui_restore(box, menu_background", source)
        self.assertNotIn("render_commander();", menu)

    def test_bidirectional_copy_and_write_opt_in_are_present(self):
        source = (ROOT / "src/kc_ui.c").read_text()
        remote = (ROOT / "src/kc_remote.c").read_text()
        host_source = (ROOT / "host/kcommander_host.py").read_text()
        self.assertIn("copy_local_to_pc", source)
        self.assertIn("copy_pc_to_local", source)
        self.assertIn("kc_remote_commit", remote)
        self.assertIn('parser.add_argument("--write"', host_source)

    def test_takeover_plan_precedes_sources(self):
        plan = (ROOT / "PLAN.md").read_text()
        self.assertIn("Phase 6 - useful file viewer and hardening", plan)
        self.assertIn("Non-negotiable invariants", plan)


if __name__ == "__main__":
    unittest.main()
