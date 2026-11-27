#!/usr/bin/env python3
"""Reference extraction of the copy/residency ledger on a small known object.

An object with two functions and one state variable is assembled, linked
the way a driver is (tools/modpack.py's layout and relocation resolver), and
the references the ledger extracts are checked against the source.

Run with: python3 -m unittest tests.test_ledger_copy_residency -v
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import warnings
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import modpack  # noqa: E402
import ledger_copy_residency as ledger  # noqa: E402

SOURCE = """\
	.area _CODE
_first::
	ld	hl,#_state
	call	_second
	ret
_second::
	ld	a,(_state+2)
	ret
_third::
	call	_missing
	ret
	.area _DRIVER_IO
_state::
	.ds	4
"""

MANIFEST = {
    "schema": "kos-driver-1", "name": "tiny", "objects": ["tiny.rel"], "init": None,
    "segments": [{"role": "hot", "image_areas": ["_CODE"]},
                 {"role": "io", "bss_areas": ["_DRIVER_IO"]}],
    "endpoints": [{"class": 12, "instance": 0, "version": 1, "dispatch": "_first", "flags": 0}],
}


def setUpModule():
    warnings.filterwarnings("ignore", category=ResourceWarning, module="modpack")


@unittest.skipUnless(shutil.which("sdasz80"), "the assembler is not installed")
class TestReferenceExtraction(unittest.TestCase):
    def build(self, tmp, source):
        asm = Path(tmp) / "tiny.asm"
        asm.write_text(source)
        subprocess.check_call(["sdasz80", "-g", "-l", "-o", str(Path(tmp) / "tiny.rel"), str(asm)])
        (Path(tmp) / "tiny.json").write_text(json.dumps(MANIFEST))
        manifest = modpack.load_manifest(str(Path(tmp) / "tiny.json"), tmp)
        model = object.__new__(ledger.DriverModel)
        model.manifest = manifest
        model.layout = modpack.build_driver_layout(manifest)
        model.symbols = modpack.build_symbol_table(manifest)
        model.row = {"hot_base": 0x6000, "io_base": 0xB000}
        return model

    def test_references_are_extracted_and_resolved(self):
        """first -> state (address of), first -> second (call), second ->
        state + 2 (load): three references, every target resolved at its
        linked address (the sdas object names its own areas, so the target
        is the area base plus the symbol's offset); a call to a symbol no object defines
        is reported as unresolved and never invented."""
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(tmp, SOURCE.replace("_third::\n\tcall\t_missing\n\tret\n", ""))
            refs, unresolved = model.references()
            self.assertEqual(unresolved, [])
            shape = sorted((r["site_role"], r["target_role"], r["target"]) for r in refs)
            self.assertEqual(shape, [("hot", "hot", 0x6007), ("hot", "io", 0xB000), ("hot", "io", 0xB002)])
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(tmp, SOURCE)
            refs, unresolved = model.references()
            self.assertEqual(len(unresolved), 1)
            self.assertIn("_missing", unresolved[0])
            self.assertEqual(len(refs), 3)


if __name__ == "__main__":
    unittest.main()
