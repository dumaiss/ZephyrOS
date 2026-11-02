#!/usr/bin/env python3
"""The Kona Executable placer (tools/exeplace.py, architecture 7.2).

The placement probe (tests/place/) pins its sources to every zone -
resident, home, far 1 and far 2 - so each kind of crossing goes through a
veneer; run on the production image it checks every result and that a
timer handler keeps running while far code does.  The rule itself is
checked on the probe's placed objects: they verify, and an object moved
into another zone without its veneer does not.  A program that fits its
bank links as written.  Code whose address is stored in data is resident
(a program's tables are found in the relocations, an SDK library's are
declared by kos.mk), and a pin elsewhere is refused.

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


def module(name, areas, symbols, words=()):
    """A relocatable module as text.  areas: [(name, size, T bytes)];
    symbols: [(name, area index or None for a reference)]; words: [(area
    index, offset, symbol name, or target area index)] address words."""
    out = ["XL4", "H %X areas %X global symbols" % (len(areas), len(symbols)),
           "M %s" % name]
    refs = [s for s, area in symbols if area is None]
    index = {}
    for symbol in refs:
        index[symbol] = len(index)
        out.append("S %s Ref0000" % symbol)
    for number, (area, size, _code) in enumerate(areas):
        out.append("A %s size %X flags 0 addr 0" % (area, size))
        for symbol, owner in symbols:
            if owner == number:
                index[symbol] = len(index)
                out.append("S %s Def0000" % symbol)
    for number, (area, size, code) in enumerate(areas):
        mine = [w for w in words if w[0] == number]
        if not mine:
            continue
        out.append("T 00 00 00 00 " + " ".join("%02X" % b for b in code))
        items = []
        for _area, offset, target in mine:
            if isinstance(target, str):
                items += [2, offset + 4, index[target] & 255, index[target] >> 8]
            else:
                items += [0, offset + 4, target, 0]
        out.append("R 00 00 %02X 00 " % number +
                   " ".join("%02X" % i for i in items))
    return exeplace.Module(name, name, "\n".join(out) + "\n")


def pointer_program(table, pin=None, declared=()):
    """A resident main that fills the resident zone but for room for one
    256-byte module: `aux.rel`, which sorts first, takes it unless `fn.rel`
    is forced in, and `fn.rel` would go to a far zone.  table:
    how `fn.rel`'s address is stored: None (not stored), "own" (an _RCONST
    word of fn.rel, area relative), "symbol" (an _RCONST word of main.rel
    naming its symbol) or "data" (a _DATA word of main.rel)."""
    room = exeplace.APERTURE - exeplace.LOW_BASE - exeplace.SDK_LOW
    main = [("_CODE", room - 256 - 4 - 32, [0, 0, 0]),
            ("_DATA", 0, []), ("_RCONST", 2, [0, 0])]
    main_words = []
    fn_areas = [("_CODE", 0x100, [0] * 4), ("_RCONST", 2, [0x10, 0x00])]
    fn_words = []
    refs = []
    if table == "symbol":
        main_words.append((2, 0, "_fn"))
        refs.append(("_fn", None))
    elif table == "data":
        main[1] = ("_DATA", 2, [0, 0])
        main_words.append((1, 0, "_fn"))
        refs.append(("_fn", None))
    elif table == "own":
        fn_words = [(1, 0, 0)]
    modules = [module("main", main, [("_main", 0)] + refs, main_words),
               module("aux", [("_CODE", 0x100, [0] * 4)], [("_aux", 0)]),
               module("fn", fn_areas, [("_fn", 0)], fn_words)]
    for m in modules:
        m.origin = m.name + ".rel"
    pins = {"main.rel": "resident"}
    if pin:
        pins["fn.rel"] = pin
    return exeplace.place(modules, 256, set(), pins, declared)[0]


class TestFunctionPointers(unittest.TestCase):
    """A function pointer stored in data may be called from any zone."""

    def zone(self, plan):
        return plan.zone[("fn.rel", "code")]

    def test_without_a_stored_address_the_module_goes_far(self):
        self.assertNotEqual(self.zone(pointer_program(None)), "resident")

    def test_a_constant_table_of_the_module_keeps_it_resident(self):
        plan = pointer_program("own")
        self.assertEqual(self.zone(plan), "resident")
        self.assertEqual(plan.why[("fn.rel", "code")], "function pointer")

    def test_a_constant_table_naming_its_symbol_keeps_it_resident(self):
        self.assertEqual(self.zone(pointer_program("symbol")), "resident")

    def test_a_data_word_keeps_it_resident(self):
        self.assertEqual(self.zone(pointer_program("data")), "resident")

    def test_pinning_it_away_is_refused_naming_the_unit_and_the_table(self):
        for table, area in (("own", "constants of fn.rel"),
                            ("symbol", "constants of main.rel"),
                            ("data", "data of main.rel")):
            with self.assertRaises(exeplace.PlaceError) as caught:
                pointer_program(table, pin="far1")
            message = str(caught.exception)
            self.assertIn("fn.rel", message)
            self.assertIn("pinned to far1", message)
            self.assertIn(area, message)

    def test_pinning_it_resident_is_accepted(self):
        self.assertEqual(self.zone(pointer_program("own", pin="resident")),
                         "resident")

    def test_a_declared_member_is_resident_and_refuses_a_pin_away(self):
        plan = pointer_program(None, declared={"fn.rel"})
        self.assertEqual(self.zone(plan), "resident")
        with self.assertRaises(exeplace.PlaceError) as caught:
            pointer_program(None, pin="home", declared={"fn.rel"})
        self.assertIn("fn.rel", str(caught.exception))
        self.assertIn("SDK declares", str(caught.exception))


KOS_MK = os.path.join(ROOT, "mk", "kos.mk")


class TestLibraryPointers(unittest.TestCase):
    """The SDK declares the library members reached through function
    pointers (mk/kos.mk): their tables sit in the library's code, where
    the relocations do not show them."""

    def declared(self):
        with open(KOS_MK) as handle:
            text = handle.read().replace("\\\n", " ")
        return {m.group(1): m.group(2).split() for m in re.finditer(
            r"^KOS_POINTER_MEMBERS_(\w+)\s*:=(.*)$", text, re.M)}

    def test_every_declared_member_is_in_its_library(self):
        libs = os.path.join(BUILD, "sdk")
        if not os.path.exists(os.path.join(libs, "libtui.lib")):
            self.skipTest("run make")
        declared = self.declared()
        self.assertIn("tui_mouse.rel", declared["tui"])
        for name, members in declared.items():
            have = [m for m, _t in exeplace.read_archive(
                os.path.join(libs, "lib%s.lib" % name))]
            for member in members:
                self.assertIn(member, have)

    def zones(self, program):
        path = os.path.join(BUILD, program, "place", "placement.json")
        if not os.path.exists(path):
            self.skipTest("run make")
        with open(path) as handle:
            placement = json.load(handle)
        with open(os.path.join(BUILD, program, "place",
                               "placement.txt")) as handle:
            report = handle.read()
        zones = {os.path.basename(p)[4:]: z
                 for p, z in placement["zones"].items()}
        return zones, report

    def test_a_pin_written_with_its_parentheses_links(self):
        """tui_mplaced pins libtui.lib(tui_mouse.rel)=resident, unquoted
        in the Makefile."""
        zones, _report = self.zones("tui_mplaced")
        self.assertEqual(zones["tui_mouse.rel"], "resident")

    def test_a_placed_program_gets_the_mouse_member_resident_unpinned(self):
        """tui_mdeclared pins only its own source: the mouse member is
        resident because the SDK declares it."""
        zones, report = self.zones("tui_mdeclared")
        self.assertEqual(zones["tui_mouse.rel"], "resident")
        self.assertRegex(report, r"code libtui\.lib\(tui_mouse\.rel\) +"
                         r"function pointer")


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
