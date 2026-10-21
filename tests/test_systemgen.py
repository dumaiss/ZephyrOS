#!/usr/bin/env python3
"""tools/systemgen.py: the image build description.

One `systems/<name>.json` describes an image, and the generator writes the
build inputs derived from it: `system.mk`, the ROMFS store manifest, the
boot manifest `drivers.man` and one resolved driver manifest per selected
driver.  These tests pin the schema rejections, the symbolic dependency
resolution, the boot-setting rules and the two generated
manifests' shape.  The golden test reproduces the
production boot manifest byte for byte.
"""

import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import systemgen  # noqa: E402

SYSTEMS = os.path.join(ROOT, "systems")

# The load order the store packs the driver images in, and the boot
# manifest that follows from it for the production image: six 24-byte
# ROMFS name records, the field formats/include/mod.h defines.
PRODUCTION_DRIVERS = ["sio", "ioc", "hid", "v9958", "console-v9958-hid",
                      "timer", "sound", "iocfat"]
PRODUCTION_MAN = b"".join(
    name.encode("ascii") + bytes(24 - len(name))
    for name in [driver + ".mod" for driver in PRODUCTION_DRIVERS])


def driver_manifest(driver_id, **overrides):
    """A minimal kos-driver-1 manifest for the temporary trees."""
    manifest = {
        "schema": "kos-driver-1",
        "name": driver_id,
        "objects": ["%s.rel" % driver_id],
        "init": "%s_init" % driver_id,
        "segments": [{"role": "hot", "image_areas": ["_CODE"]},
                     {"role": "io", "bss_areas": ["_DRIVER_IO"]}],
        "endpoints": [{"class": 16, "instance": 0, "version": 1,
                       "dispatch": "%s_dispatch" % driver_id, "flags": 0}],
        "publishes": [],
        "depends": [],
    }
    manifest.update(overrides)
    return manifest


class TreeCase(unittest.TestCase):
    """A temporary repo-shaped tree the generator runs against."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="systemgen-")
        self.addCleanup(shutil.rmtree, self.tmp)
        os.makedirs(os.path.join(self.tmp, "systems"))
        os.makedirs(os.path.join(self.tmp, "drivers"))
        self.build = os.path.join(self.tmp, "build")

    def driver(self, driver_id, where=None, **overrides):
        directory = os.path.join(self.tmp, where or os.path.join(
            "drivers", driver_id))
        os.makedirs(directory, exist_ok=True)
        path = os.path.join(directory, "%s.json" % driver_id)
        with open(path, "w") as handle:
            json.dump(driver_manifest(driver_id, **overrides), handle)
        return path

    def system(self, name="probe", **overrides):
        document = {"schema": "kos-system-1", "name": name,
                    "drivers": [], "boot": "/system/monitor.exe",
                    "after_exit": "monitor", "store": ["monitor.exe"],
                    "cflags": []}
        document.update(overrides)
        path = os.path.join(self.tmp, "systems", "%s.json" % name)
        with open(path, "w") as handle:
            json.dump(document, handle)
        return path

    def run_generator(self, path, *args, **kwargs):
        return systemgen.generate(path, self.build, *args, **kwargs)

    def generated(self, name):
        with open(os.path.join(self.build, name), encoding="utf-8") as handle:
            return json.load(handle)

    def boot_manifest(self):
        with open(os.path.join(self.build, "modules", "drivers.man"),
                  "rb") as handle:
            return handle.read()

    def system_mk(self):
        with open(os.path.join(self.build, "system.mk"),
                  encoding="utf-8") as handle:
            return handle.read()


class TestSchemas(TreeCase):
    def test_rejects_a_non_system_schema(self):
        path = self.system(schema="kos-system-2")
        with self.assertRaisesRegex(systemgen.SystemError, "schema"):
            self.run_generator(path)

    def test_rejects_an_unknown_field(self):
        path = self.system(extra_field=1)
        with self.assertRaisesRegex(systemgen.SystemError, "unknown field"):
            self.run_generator(path)

    def test_rejects_a_name_that_is_not_the_file_name(self):
        path = self.system(name="probe")
        renamed = os.path.join(self.tmp, "systems", "other.json")
        shutil.move(path, renamed)
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "systems/<name>.json"):
            self.run_generator(renamed)

    def test_rejects_a_driver_without_the_driver_schema(self):
        self.driver("one", schema="kos-driver-0")
        path = self.system(drivers=["one"])
        with self.assertRaisesRegex(systemgen.SystemError, "kos-driver-1"):
            self.run_generator(path)

    def test_rejects_a_driver_name_that_is_not_its_id(self):
        self.driver("one", name="two")
        path = self.system(drivers=["one"])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "is not the driver id"):
            self.run_generator(path)

    def test_rejects_a_cflags_entry_that_names_an_exe(self):
        self.driver("one")
        path = self.system(drivers=["one"], cflags=["-DKOS_BOOT_PROGRAM=/system/x.exe"])
        with self.assertRaisesRegex(systemgen.SystemError, "\\.exe"):
            self.run_generator(path)


class TestDriverSelection(TreeCase):
    def test_rejects_an_unknown_driver(self):
        path = self.system(drivers=["nosuch"])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "unknown driver 'nosuch'"):
            self.run_generator(path)

    def test_rejects_a_driver_listed_twice(self):
        self.driver("one")
        path = self.system(drivers=["one", "one"])
        with self.assertRaisesRegex(systemgen.SystemError, "twice"):
            self.run_generator(path)

    def test_rejects_an_ambiguous_bare_id(self):
        self.driver("one", where=os.path.join("drivers", "a"))
        self.driver("one", where=os.path.join("drivers", "b"))
        path = self.system(drivers=["one"])
        with self.assertRaisesRegex(systemgen.SystemError, "ambiguous"):
            self.run_generator(path)

    def test_resolves_a_shared_driver_directory(self):
        self.driver("shared", where=os.path.join("drivers", "console"))
        path = self.system(drivers=["shared"])
        self.assertEqual(self.run_generator(path), 0)
        self.assertTrue(os.path.isfile(
            os.path.join(self.build, "drivers", "shared.json")))

    def test_resolves_a_test_only_driver_by_path(self):
        self.driver("probe", where=os.path.join("tests", "ns", "manifests"))
        path = self.system(drivers=["tests/ns/manifests/probe"])
        self.assertEqual(self.run_generator(path), 0)
        self.assertTrue(os.path.isfile(
            os.path.join(self.build, "drivers", "probe.json")))


class TestDependencies(TreeCase):
    def test_resolves_a_type_name_to_the_selected_endpoint(self):
        self.driver("ioc", endpoints=[{"class": 5, "instance": 0,
                                       "version": 1, "dispatch": "d",
                                       "flags": 0}])
        self.driver("hid", depends=["IOC"])
        path = self.system(drivers=["ioc", "hid"])
        self.assertEqual(self.run_generator(path), 0)
        resolved = self.generated(os.path.join("drivers", "hid.json"))
        self.assertEqual(resolved["depends"],
                         [{"class": 5, "instance": 0}])

    def test_resolves_an_instance(self):
        self.driver("video", endpoints=[{"class": 2, "instance": 1,
                                         "version": 1, "dispatch": "d",
                                         "flags": 0}])
        self.driver("app", depends=["VIDEO:1"])
        path = self.system(drivers=["video", "app"])
        self.assertEqual(self.run_generator(path), 0)
        resolved = self.generated(os.path.join("drivers", "app.json"))
        self.assertEqual(resolved["depends"],
                         [{"class": 2, "instance": 1}])

    def test_resolves_a_raw_test_class(self):
        self.driver("echo", endpoints=[{"class": 16, "instance": 0,
                                        "version": 1, "dispatch": "d",
                                        "flags": 0}])
        self.driver("user", endpoints=[], depends=["#16"])
        path = self.system(drivers=["echo", "user"])
        self.assertEqual(self.run_generator(path), 0)
        resolved = self.generated(os.path.join("drivers", "user.json"))
        self.assertEqual(resolved["depends"],
                         [{"class": 16, "instance": 0}])

    def test_a_missing_dependency_names_the_system_driver_and_entry(self):
        self.driver("app", depends=["VIDEO"])
        path = self.system(drivers=["app"])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "probe: app: dependency 'VIDEO' matches "
                                    "no selected endpoint"):
            self.run_generator(path)

    def test_an_ambiguous_dependency_names_the_providers(self):
        for name in ("hid", "other"):
            self.driver(name, endpoints=[{"class": 4, "instance": 0,
                                          "version": 1, "dispatch": "d",
                                          "flags": 0}])
        self.driver("app", depends=["INPUT"])
        path = self.system(drivers=["hid", "other", "app"])
        with self.assertRaisesRegex(
                systemgen.SystemError,
                "app: dependency 'INPUT' matches more than one selected "
                "endpoint: hid, other"):
            self.run_generator(path)

    def test_an_unmatched_optional_dependency_keeps_its_row(self):
        self.driver("app", depends=["VIDEO?"])
        path = self.system(drivers=["app"])
        self.assertEqual(self.run_generator(path), 0)
        resolved = self.generated(os.path.join("drivers", "app.json"))
        self.assertEqual(resolved["depends"],
                         [{"class": 2, "instance": 0, "optional": 1}])

    def test_rejects_a_name_that_is_not_a_type_or_a_class(self):
        self.driver("app", depends=["lowercase"])
        path = self.system(drivers=["app"])
        with self.assertRaisesRegex(systemgen.SystemError, "depends\\[0\\]"):
            self.run_generator(path)


class TestBootSetting(TreeCase):
    def test_rejects_a_path_outside_system(self):
        self.driver("one")
        path = self.system(drivers=["one"], boot="/boot/monitor.exe")
        with self.assertRaisesRegex(systemgen.SystemError, "under /system"):
            self.run_generator(path)

    def test_rejects_an_unknown_after_exit_policy(self):
        self.driver("one")
        path = self.system(drivers=["one"], after_exit="loop")
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "after_exit must be one of"):
            self.run_generator(path)

    def test_rejects_a_monitor_image_the_store_cannot_serve(self):
        self.driver("one")
        path = self.system(drivers=["one"], boot="/system/shell.exe",
                           store=["shell.exe"])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "monitor.exe is not in the system's "
                                    "store"):
            self.run_generator(path)

    def test_accepts_a_monitor_image_that_carries_it(self):
        self.driver("one")
        path = self.system(drivers=["one"])
        self.assertEqual(self.run_generator(path), 0)

    def test_rejects_a_boot_program_the_store_does_not_carry(self):
        self.driver("one")
        path = self.system(drivers=["one"], boot="/system/absent.exe")
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "absent.exe is not in the system's "
                                    "store"):
            self.run_generator(path)

    def test_rejects_a_store_entry_that_is_not_an_exe(self):
        self.driver("one")
        path = self.system(drivers=["one"], store=["monitor.exe"],
                           boot="/system/drivers.man")
        with self.assertRaisesRegex(systemgen.SystemError, "not an EXE"):
            self.run_generator(path)

    def test_rejects_a_boot_name_over_the_romfs_field(self):
        # The boot program is one of the store's flat names, and a store
        # name holds at most 23 characters: a longer boot name is caught
        # by the store rule before the path-length rule can fire.
        self.driver("one")
        name = "m" * 24 + ".exe"
        path = self.system(drivers=["one"], store=[name],
                           boot="/system/" + name)
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "at most 23 characters"):
            self.run_generator(path)

    def test_rejects_a_path_with_a_space(self):
        self.driver("one")
        path = self.system(drivers=["one"], store=["a b.exe"],
                           boot="/system/a b.exe")
        with self.assertRaisesRegex(systemgen.SystemError, "printable ASCII"):
            self.run_generator(path)

    def test_accepts_a_boot_program_the_store_carries(self):
        self.driver("one")
        path = self.system(drivers=["one"], store=["monitor.exe"])
        self.assertEqual(self.run_generator(path), 0)


class TestGeneratedFiles(TreeCase):
    def test_store_manifest_order_is_drivers_then_man_then_store(self):
        self.driver("one")
        self.driver("two")
        path = self.system(drivers=["one", "two"],
                           store=["monitor.exe", "crosspage.bin"])
        self.assertEqual(self.run_generator(path), 0)
        store = self.generated("store.json")
        self.assertEqual(store["format"], "romfs-v1")
        self.assertEqual([entry["name"] for entry in store["files"]],
                         ["one.mod", "two.mod", "drivers.man",
                          "monitor.exe", "crosspage.bin"])
        self.assertEqual([entry["type"] for entry in store["files"]],
                         ["MOD", "MOD", "DATA", "EXE", "DATA"])

    def test_boot_manifest_holds_one_record_per_selected_driver(self):
        self.driver("one")
        self.driver("console-two", where=os.path.join("drivers", "console"))
        path = self.system(drivers=["one", "console-two"])
        self.assertEqual(self.run_generator(path), 0)
        self.assertEqual(self.boot_manifest(),
                         b"one.mod".ljust(24, b"\0") +
                         b"console-two.mod".ljust(24, b"\0"))

    def test_the_store_rejects_a_duplicate_name(self):
        self.driver("monitor")
        path = self.system(drivers=["monitor"],
                           store=["monitor.mod", "monitor.exe"])
        with self.assertRaisesRegex(systemgen.SystemError, "appears twice"):
            self.run_generator(path)

    def test_system_mk_carries_the_system_and_the_object_list(self):
        self.driver("one", objects=["one.rel", "one_io.rel"])
        path = self.system(drivers=["one"], cflags=["-DONE=1"])
        self.assertEqual(self.run_generator(path), 0)
        text = self.system_mk()
        self.assertIn("KOS_DRIVERS := one", text)
        self.assertIn("KOS_BOOT_PROGRAM := /system/monitor.exe", text)
        self.assertIn("KOS_SYSTEM_CFLAGS := -DONE=1", text)
        self.assertIn("modules/one.rel", text)
        self.assertIn("modules/one_io.rel", text)

    def test_omit_image_leaves_the_name_in_the_boot_manifest(self):
        self.driver("one")
        self.driver("quiet")
        path = self.system(drivers=["one", "quiet"])
        self.assertEqual(self.run_generator(path, omit=["quiet"]), 0)
        store = self.generated("store.json")
        self.assertEqual([entry["name"] for entry in store["files"]],
                         ["one.mod", "drivers.man", "monitor.exe"])
        self.assertEqual(self.boot_manifest(),
                         b"one.mod".ljust(24, b"\0") +
                         b"quiet.mod".ljust(24, b"\0"))

    def test_omit_image_must_name_a_selected_driver(self):
        self.driver("one")
        path = self.system(drivers=["one"])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "names no selected driver"):
            self.run_generator(path, omit=["absent"])


class TestTheRealSystems(unittest.TestCase):
    """The checked-in systems/ files, generated into a temporary build
    directory: the golden boot manifest and the shipped store orders."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="systemgen-real-")
        self.addCleanup(shutil.rmtree, self.tmp)

    def generate(self, name, *args, **kwargs):
        path = os.path.join(SYSTEMS, "%s.json" % name)
        return systemgen.generate(path, self.tmp, *args, **kwargs)

    def store(self):
        with open(os.path.join(self.tmp, "store.json"),
                  encoding="utf-8") as handle:
            return json.load(handle)

    def test_production_reproduces_the_golden_boot_manifest(self):
        self.assertEqual(self.generate("production"), 0)
        with open(os.path.join(self.tmp, "modules", "drivers.man"),
                  "rb") as handle:
            self.assertEqual(handle.read(), PRODUCTION_MAN)

    def test_production_store_is_the_canonical_order(self):
        self.assertEqual(self.generate("production"), 0)
        self.assertEqual([entry["name"] for entry in self.store()["files"]],
                         [driver + ".mod" for driver in PRODUCTION_DRIVERS] +
                         ["drivers.man", "shell.exe", "monitor.exe",
                          "hwtest.exe", "timer.exe", "beat.exe",
                          "xmem_selftest.exe",
                          "xmem_child.exe",
                          "xmem_far.exe", "fs_selftest.exe",
                          "fs_child.exe"])

    def test_production_resolves_the_console_dependencies(self):
        self.assertEqual(self.generate("production"), 0)
        with open(os.path.join(self.tmp, "drivers",
                               "console-v9958-hid.json"),
                  encoding="utf-8") as handle:
            console = json.load(handle)
        self.assertEqual(console["depends"],
                         [{"class": 4, "instance": 0},
                          {"class": 2, "instance": 0},
                          {"class": 1, "instance": 0}])

    def test_the_missing_image_store_leaves_the_driver_out(self):
        self.assertEqual(self.generate("console-serial-missing",
                                       omit=["console-serial"]), 0)
        names = [entry["name"] for entry in self.store()["files"]]
        self.assertNotIn("console-serial.mod", names)
        with open(os.path.join(self.tmp, "modules", "drivers.man"),
                  "rb") as handle:
            manifest = handle.read()
        self.assertIn(b"console-serial.mod", manifest)

    def test_every_system_file_generates(self):
        for name in sorted(os.listdir(SYSTEMS)):
            system = name[:-len(".json")]
            with self.subTest(system=system):
                shutil.rmtree(self.tmp)
                os.makedirs(self.tmp)
                extra = {"omit": ["console-serial"]} \
                    if system == "console-serial-missing" else {}
                self.assertEqual(self.generate(system, **extra), 0, system)

    def test_every_driver_manifest_has_the_current_schema(self):
        for directory, _dirs, names in os.walk(os.path.join(ROOT, "drivers")):
            for name in names:
                if not name.endswith(".json"):
                    continue
                path = os.path.join(directory, name)
                with open(path, encoding="utf-8") as handle:
                    manifest = json.load(handle)
                self.assertEqual(manifest.get("schema"), "kos-driver-1", path)
                self.assertIsInstance(manifest.get("depends"), list, path)
                for entry in manifest["depends"]:
                    self.assertIsInstance(entry, str, path)


if __name__ == "__main__":
    unittest.main()
