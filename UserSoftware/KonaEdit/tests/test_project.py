"""Static/package checks for the standalone KonaEdit consumer."""

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "build/user/konaedit/konaedit.report.json"


class KonaEditProjectTest(unittest.TestCase):
    def test_document_limits_are_32_bit_and_exact(self):
        types = (ROOT / "src/ke_types.h").read_text()
        self.assertRegex(types, r"typedef\s+uint32_t\s+ke_pos_t")
        self.assertRegex(types, r"KE_DOCUMENT_CAPACITY\s+65536ul")
        self.assertRegex(types, r"KE_PAGE_COUNT\s+64u")
        self.assertRegex(types, r"KE_OBJECT_COUNT\s+8u")

    def test_target_uses_only_public_sdk_surface(self):
        makefile = (ROOT / "Makefile").read_text()
        sources = "\n".join(
            path.read_text(errors="replace")
            for path in sorted((ROOT / "src").glob("*.[ch]"))
        )
        self.assertIn("include $(KOS_SDK_ROOT)/mk/kos.mk", makefile)
        self.assertIn("LIBS=tui", makefile)
        self.assertNotRegex(sources, r'#include\s+"(?:kernel|drivers|lib)/')
        self.assertNotIn("\\033", sources)
        self.assertNotIn("console_write", sources)

    def test_packed_image_keeps_required_stack_headroom(self):
        if not REPORT.is_file():
            self.skipTest("build KonaEdit before checking its pack report")
        report = json.loads(REPORT.read_text())
        linked_end = 0x0100 + report["image"]["size"] + report["bss"]["size"]
        headroom = report["stack"]["base"] - linked_end
        self.assertGreaterEqual(report["stack"]["size"], 768)
        self.assertGreaterEqual(headroom, 512)

    def test_required_operator_documents_exist(self):
        for name in ("README.md", "DESIGN.md"):
            text = (ROOT / name).read_text()
            self.assertGreater(len(text), 500, name)


if __name__ == "__main__":
    unittest.main()
