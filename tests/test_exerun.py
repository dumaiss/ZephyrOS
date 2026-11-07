#!/usr/bin/env python3
"""tools/exerun.py, the SDK's `make run` helper, without an emulator.

* card names: the program as <NAME>.EXE and its assets as valid 8.3 names,
  an explicit NAME kept only when it already is one, two files under one
  name refused;
* the inputs a run needs are reported by name and nothing is staged or
  started without them: the flash image, the MAME binary (a path, or a
  name on PATH), the konaos plugin beside it or in KOS_MAME_PLUGINS;
* an emulator bundle's directory as the emulator: its `mamepbitz`, `plugins/`
  and `konaos.bin` need no other setting.
"""

import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))

import exerun  # noqa: E402


class TestCardNames(unittest.TestCase):

    def test_short_names(self):
        for name, card in (("hello.exe", "HELLO.EXE"), ("tui_dialog.exe", "TUI_DIAL.EXE"),
                           ("night-market.ztr", "NIGHT-MA.ZTR"), ("a.b.c", "AB.C"),
                           ("my file.text", "MYFILE.TEX"), ("README", "README")):
            self.assertEqual(exerun.short_name(name), card, name)

    def test_a_name_without_usable_characters_is_refused(self):
        with self.assertRaises(exerun.RunError):
            exerun.short_name("+++.txt")

    def test_staging(self):
        with tempfile.TemporaryDirectory(prefix="exe run ") as tmp:
            tmp = Path(tmp)
            program = tmp / "tui_dialog.exe"
            notes = tmp / "notes.txt"
            other = tmp / "other notes.txt"
            for path in (program, notes, other):
                path.write_bytes(b"x")
            plan = exerun.staging(program, [str(notes), "%s=OTHER.TXT" % other])
            self.assertEqual([name for _, name in plan], ["TUI_DIAL.EXE", "NOTES.TXT", "OTHER.TXT"])
            with self.assertRaisesRegex(exerun.RunError, "not a valid 8.3 name"):
                exerun.staging(program, ["%s=notes-long.txt" % notes])
            with self.assertRaisesRegex(exerun.RunError, "would both be NOTES.TXT"):
                exerun.staging(program, [str(notes), "%s=NOTES.TXT" % other])
            with self.assertRaisesRegex(exerun.RunError, "missing"):
                exerun.staging(program, [str(tmp / "absent.bin")])


class TestInputs(unittest.TestCase):

    def run_main(self, *args):
        err, out = StringIO(), StringIO()
        with redirect_stderr(err), redirect_stdout(out):
            code = exerun.main(list(args))
        return code, err.getvalue()

    def test_missing_inputs_are_named_and_nothing_starts(self):
        with tempfile.TemporaryDirectory(prefix="exe run ") as tmp:
            tmp = Path(tmp)
            exe = tmp / "hello.exe"
            exe.write_bytes(b"x")
            flash = tmp / "kona os.bin"
            flash.write_bytes(b"\xff")
            card = tmp / "the card"
            common = ["--exe", str(exe), "--card", str(card), "--rundir", str(tmp / "run")]
            code, err = self.run_main(*common)
            self.assertEqual(code, 2)
            self.assertIn("set KOS_FLASH", err)
            code, err = self.run_main(*common, "--flash", str(flash), "--mame", str(tmp / "no mame"))
            self.assertEqual(code, 2)
            self.assertIn("set KOS_MAME", err)
            saved = os.environ.get("PATH", "")
            os.environ["PATH"] = str(tmp)
            try:
                code, err = self.run_main(*common, "--flash", str(flash))
            finally:
                os.environ["PATH"] = saved
            self.assertEqual(code, 2)
            self.assertIn("'mamepbitz' is not found", err)
            mame = tmp / "mame bin"
            mame.write_text("#!/bin/sh\nexit 0\n")
            mame.chmod(0o755)
            code, err = self.run_main(*common, "--flash", str(flash), "--mame", str(mame))
            self.assertEqual(code, 2)
            self.assertIn("konaos MAME plugin is not in", err)
            self.assertFalse(card.exists(), "a refused run staged files")

    def test_a_bundle_directory_is_the_whole_emulator(self):
        with tempfile.TemporaryDirectory(prefix="exe run ") as tmp:
            tmp = Path(tmp)
            exe = tmp / "hello.exe"
            exe.write_bytes(b"x")
            bundle = tmp / "emulator bundle"
            (bundle / "plugins/konaos").mkdir(parents=True)
            (bundle / "plugins/konaos/init.lua").write_text("")
            (bundle / "konaos.bin").write_bytes(b"\xff")
            common = ["--exe", str(exe), "--card", str(tmp / "the card"), "--rundir", str(tmp / "run"),
                      "--mame", str(bundle)]
            code, err = self.run_main(*common)
            self.assertEqual(code, 2)
            self.assertIn("is not found", err)
            mame = bundle / "mamepbitz"
            mame.write_text("#!/bin/sh\nexit 0\n")
            mame.chmod(0o755)
            code, err = self.run_main(*common)
            self.assertEqual(code, 0, err)
            self.assertTrue((tmp / "the card/HELLO.EXE").is_file())
            self.assertTrue((tmp / "run/session").is_file())
            (bundle / "konaos.bin").unlink()
            code, err = self.run_main(*common)
            self.assertEqual(code, 2)
            self.assertIn("konaos.bin is missing", err)


if __name__ == "__main__":
    unittest.main()
