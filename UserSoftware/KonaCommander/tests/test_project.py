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
