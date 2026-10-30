#!/usr/bin/env python3
"""The Kona Executable placer (tools/exeplace.py, architecture 7.2).

The placement probe (tests/place/) pins its sources to every zone -
resident, home, far 1 and far 2 - so each kind of crossing goes through a
veneer; run on the production image it checks every result and that a
timer handler keeps running while far code does.  The rule itself is
checked on the probe's placed objects: they verify, and an object moved
into another zone without its veneer does not.  A program that fits its
bank links as written.

Run with: `python3 -m unittest tests.test_exeplace`.
"""

import json
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exeplace  # noqa: E402
from test_hwtest_sim import HwtestReplay  # noqa: E402

BUILD = os.path.join(ROOT, "build", "user")
PLACETEST = os.path.join(BUILD, "placetest.exe")
PLACED = os.path.join(BUILD, "placetest", "place")
CHECKS = ("home to far1", "home to far1, stack arguments",
          "home to far2, 8 bits", "far1 to far2 and home",
          "far2 through a resident pointer", "handler while far1 runs")


@unittest.skipUnless(os.path.exists(PLACETEST), "run make")
class TestPlacedProgram(HwtestReplay):
    def test_every_crossing_holds_on_the_production_image(self):
        with open(PLACETEST, "rb") as handle:
            program = handle.read()
        z = self.start(True, {"PLACE.EXE": program})
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/place.exe\r")
        self.run_to_pattern(z, "placetest ", since=since, limit=80_000_000)
        self.run_to_pattern(z, "\n/> ", since=since, limit=80_000_000)
        transcript = "".join(z.glyphs[since:])
        for name in CHECKS:
            self.assertIn("place: %s ok" % name, transcript)
        self.assertIn("placetest PASS", transcript)


@unittest.skipUnless(os.path.exists(PLACETEST), "run make")
class TestPlacement(unittest.TestCase):
    def placement(self):
        with open(os.path.join(PLACED, "placement.json")) as handle:
            return json.load(handle)

    def test_the_pins_and_the_veneers(self):
        placement = self.placement()
        zones = {os.path.basename(path)[4:]: zone
                 for path, zone in placement["zones"].items()}
        self.assertEqual(zones["place_res.rel"], "resident")
        self.assertEqual(zones["place_main.rel"], "home")
        self.assertEqual(zones["place_far1.rel"], "far1")
        self.assertEqual(zones["place_far2.rel"], "far2")
        self.assertEqual([s["area"] for s in placement["segments"]],
                         ["_FAR1", "_FAR2"])
        with open(os.path.join(PLACED, "placement.txt")) as handle:
            report = handle.read()
        for target in ("_main -> home", "_f1_mix -> far1",
                       "_f2_apply -> far2", "_home_square -> home"):
            self.assertIn(target, report)

    def test_the_placed_objects_keep_the_rule(self):
        self.assertEqual(exeplace.verify(self.placement()["objects"]), [])

    def test_a_direct_crossing_breaks_the_rule(self):
        """Far 2's object moved into far 1's area: far 1's direct call
        into it is still fine, but its own call to the home zone is not
        a veneer, and main's veneer target becomes a direct far 1 call
        from the home zone - the rule names them."""
        objects = self.placement()["objects"]
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        moved = []
        for path in objects:
            target = os.path.join(tmp, os.path.basename(path))
            with open(path, "rb") as handle:
                text = handle.read().decode("latin-1")
            if path.endswith("place_main.rel"):
                # main calls f1_add directly instead of through its veneer
                text = text.replace("S __vn_f1_add Ref", "S _f1_add Ref")
            with open(target, "wb") as handle:
                handle.write(text.encode("latin-1"))
            moved.append(target)
        problems = exeplace.verify(moved)
        self.assertTrue(any("_f1_add from home into far1" in p
                            for p in problems), problems)


class TestUnplaced(unittest.TestCase):
    def test_a_program_that_fits_links_as_written(self):
        lk = os.path.join(BUILD, "gfxtest", "gfxtest.lk")
        if not os.path.exists(lk):
            self.skipTest("run make")
        with open(os.path.join(BUILD, "gfxtest", "place",
                               "placed.lk")) as handle:
            placed = handle.read()
        with open(lk) as handle:
            self.assertEqual(handle.read(), placed)
        with open(os.path.join(BUILD, "gfxtest", "place",
                               "placement.json")) as handle:
            self.assertIsNone(json.load(handle))


if __name__ == "__main__":
    unittest.main()
