"""Host checks for the M10 namespace acceptance package (42a).

Pins the structural facts the `ns-selftest` image, its client program and
its replay depend on, without booting anything:

  - the build wiring (target, the `ZOS_NS_HANDLES` guard define, image
    names, the service set and its manifest directory, the boot program);
  - the pinned ABI: the four syscall numbers, the argument blocks and the
    namespace entry in abi/zos/object.h, the 8-bit protocol types, the two
    standard handle numbers;
  - production keeps the stage-3 pair: `ZOS_SYSCALL_IPC_LOOKUP` and the
    `ZOS_NS_HANDLES 0` default are what makes the production image
    byte-identical;
  - the container ABI 4 publication table: the section's reserved-byte
    rule, the 20-byte record, the packer's parse/plan/emit and the three
    packed containers of the image (ABI byte, names, protocol types and
    host-resolved endpoint numbers);
  - the image's service manifests and store, and `services.man` holding
    one 24-byte record per loaded image in load order;
  - the residency classification: the three new objects are `cold` and
    test-linked, so production links none of them;
  - the SDK variant: the guarded bind/unbind/ns_enum declarations and the
    gate blocks the image's zex programs link.

Run with: `python3 -m unittest tests.test_ns_selftest`.
"""

import json
import os
import re
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import mapcheck  # noqa: E402
import romfspack  # noqa: E402

MAKEFILE = os.path.join(ROOT, "Makefile")
OBJECT_ABI = os.path.join(ROOT, "abi", "zos", "object.h")
IPC_ABI = os.path.join(ROOT, "abi", "zos", "ipc.h")
SERVICE_HEADER = os.path.join(ROOT, "include", "zephyros", "service.h")
PACKER = os.path.join(ROOT, "tools", "zmodpack.py")
RESIDENCY = os.path.join(ROOT, "config", "residency.json")
LINKER = os.path.join(ROOT, "linker", "zephyr80.lk")
SDK_HEADER = os.path.join(ROOT, "user", "sdk", "include", "zos", "ipc.h")
SDK_SOURCE = os.path.join(ROOT, "user", "sdk", "src", "ipc.c")
SDK_GATE = os.path.join(ROOT, "user", "sdk", "src", "nsgate.asm")
ZEX_MK = os.path.join(ROOT, "user", "sdk", "zex.mk")
CLIENT = os.path.join(ROOT, "tests", "ns", "ns.c")
CHILD = os.path.join(ROOT, "tests", "ns", "nschild.c")
MANIFEST = os.path.join(ROOT, "tests", "ns", "manifest.json")
SERVICE_MANIFESTS = os.path.join(ROOT, "tests", "ns", "manifests")
NSDEV = os.path.join(ROOT, "tests", "target", "ns", "nsdev.c")

IMAGE_SUBDIR = os.path.join(ROOT, "build-ns-selftest")
PROD_SUBDIR = os.path.join(ROOT, "build")
MODULE_DIR = os.path.join(IMAGE_SUBDIR, "modules")

# The syscall numbers and the protocol types abi/zos/object.h pins.
SYSCALLS = {"ZOS_SYSCALL_BIND": "0x06u", "ZOS_SYSCALL_CALL": "0x07u",
            "ZOS_SYSCALL_UNBIND": "0x08u", "ZOS_SYSCALL_NS_ENUM": "0x09u"}
TYPES = {"ZOS_TYPE_SERIAL": "1u", "ZOS_TYPE_VIDEO": "2u",
         "ZOS_TYPE_BLOCK": "3u", "ZOS_TYPE_INPUT": "4u",
         "ZOS_TYPE_IOC": "5u", "ZOS_TYPE_CONSOLE": "6u",
         "ZOS_TYPE_FILE": "7u", "ZOS_TYPE_FS": "8u",
         "ZOS_TYPE_SOUND": "9u", "ZOS_TYPE_TIMER": "10u"}
HANDLES = {"ZOS_HANDLE_STDOUT": "1u", "ZOS_HANDLE_STDIN": "2u",
           "ZOS_HANDLE_FIRST_BOUND": "3u"}

# The publication table the three packed containers must carry: name below
# the DEVICE root, protocol type, serving endpoint (the host assigns them
# in load order: sio, console, nsdev) and the provider's object identity.
PUBLICATIONS = {
    "sio": [("/serial/0", 1, 1, 0)],
    "console": [("/console", 6, 2, 0)],
    "nsdev": [("/input/keyboard", 4, 3, 1), ("/video", 2, 3, 2)],
}
SERVICE_ORDER = ["sio", "console", "nsdev"]

# The store the image boots from, in manifest order; `services.man` is the
# loader's own boot manifest, one 24-byte record per loaded image.
STORE = ["sio.zmod", "console.zmod", "nsdev.zmod", "services.man",
         "ns.zex", "nschild.zex"]
MAN_RECORD = 24
# The namespace leaves the M9 mount pass without a `/` mount to check.
SPLIT = 0x6000


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def symbols(map_path):
    found = {}
    with open(map_path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"\s*([0-9A-Fa-f]{4,8})\s+(\S+)(?:\s+\S+)?\s*$",
                             line)
            if match:
                found[match.group(2)] = int(match.group(1), 16)
    return found


class TestNsBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = read(MAKEFILE)
        self.assertIn("ns-selftest:", text)
        self.assertIn("ZOS_NS_SELFTEST=1", text)
        self.assertIn("zephyros-ns-selftest.bin", text)
        self.assertIn("zephyr80-ns-selftest.bin", text)
        # The guard the image defines is the one abi/zos/object.h defaults.
        self.assertIn("-DZOS_NS_HANDLES=1", text)
        self.assertIn("test_ns_selftest*.py", text)

    def test_makefile_selects_the_image_configuration(self):
        text = read(MAKEFILE)
        for token in ("ZOS_SERVICES := sio console nsdev",
                      "SVC_MANIFEST_DIR := tests/ns/manifests",
                      "ZOS_BOOT_PROGRAM := /SYSTEM/ns.zex",
                      "ZOS_ARENA_MODULES :=",
                      "NS_OBJS := $(BUILD)/vfs_ns.rel "
                      "$(BUILD)/vfs_ns_file.rel $(BUILD)/object.rel"):
            self.assertIn(token, text, token)
        # The image links the namespace objects cold, in the placement
        # production will use (config/residency.json tier `cold`).
        self.assertIn("COLD_EXTRA_CSV := vfs_ns,vfs_ns_file,object,vfs_file",
                      text)

    def test_client_programs_exist_and_are_wired(self):
        for path in (CLIENT, CHILD, NSDEV, MANIFEST):
            self.assertTrue(os.path.exists(path), path)
        text = read(MAKEFILE)
        self.assertIn("$(call zex_program,ns,", text)
        self.assertIn("$(call zex_program,nschild,", text)
        self.assertIn("nsdev", text)


class TestNsAbi(unittest.TestCase):
    def test_object_header_pins_the_family(self):
        text = read(OBJECT_ABI)
        for name, value in SYSCALLS.items():
            self.assertRegex(text, r"#define %s\s+%s" % (name, value))
        for name, value in TYPES.items():
            self.assertRegex(text, r"#define %s\s+%s" % (name, value))
        for name, value in HANDLES.items():
            self.assertRegex(text, r"#define %s\s+%s" % (name, value))
        self.assertRegex(text, r"#define ZOS_NS_NAME_MAX\s+24u")
        self.assertRegex(text, r"#define ZOS_NS_ENTRY_SIZE\s+28u")
        self.assertRegex(text, r"#define ZOS_NS_ENTRY_CONTAINER\s+0x01u")

    def test_argument_blocks_keep_their_sizes(self):
        text = read(OBJECT_ABI)
        # The static assertions are the ABI: 8-byte bind, 1-byte unbind,
        # 12-byte ns_enum and 28-byte entry with `name` at 4.
        self.assertIn("bind argument block must be 8 packed bytes", text)
        self.assertIn("unbind argument block must be 1 packed byte", text)
        self.assertIn("ns_enum argument block must be 12 packed bytes", text)
        self.assertIn("namespace entry must be 28 packed bytes", text)
        self.assertIn('__builtin_offsetof(struct zos_ns_entry, name) == 4',
                      text)
        self.assertIn('__builtin_offsetof(struct zos_bind_args, type) == 6',
                      text)
        self.assertIn('__builtin_offsetof(struct zos_ns_enum_args, cookie) '
                      '== 4', text)
        self.assertIn('__builtin_offsetof(struct zos_ns_enum_args, count) '
                      '== 10', text)

    def test_production_keeps_the_stage_three_pair(self):
        # The acceptance image builds bind/call/unbind/ns_enum over 06h-09h;
        # production still dispatches SYS_IPC_LOOKUP through 06h until
        # package 42b, which is what keeps it byte-identical.
        text = read(OBJECT_ABI)
        self.assertIn("#define ZOS_NS_HANDLES 0", text)
        self.assertIn("ZOS_SYSCALL_IPC_LOOKUP", text)
        ipc = read(IPC_ABI)
        self.assertRegex(ipc, r"#define ZOS_SYSCALL_IPC_LOOKUP\s+0x06u")


class TestNsContainers(unittest.TestCase):
    def test_service_header_declares_abi_four(self):
        text = read(SERVICE_HEADER)
        self.assertRegex(text, r"#define ZMOD_SERVICE_ABI_VERSION\s+3u")
        self.assertRegex(text, r"#define ZMOD_SERVICE_ABI_PUBLICATIONS\s+4u")
        self.assertRegex(text,
                         r"#define ZMOD_SERVICE_PUBLICATION_SIZE\s+20u")
        self.assertRegex(text,
                         r"#define ZMOD_SERVICE_MAX_PUBLICATIONS\s+12u")
        # ABI 4 lives in the section's reserved bytes: the byte at 0Bh is
        # the count, the little-endian word at 0Ch the table offset.
        self.assertIn("0Bh) is the publication count", text)
        self.assertIn("publication table's offset", text)
        self.assertIn("publication must be 20 bytes", text)

    def test_packer_emits_and_validates_the_table(self):
        text = read(PACKER)
        for token in ("SERVICE_ABI_PUBLICATIONS = 4",
                      "SERVICE_PUBLICATION_SIZE = 20",
                      "SERVICE_PUBLICATION_NAME = 16",
                      "SERVICE_MAX_PUBLICATIONS = 12",
                      "publishes", "SERVICE_DEVICE_ROOT"):
            self.assertIn(token, text, token)
        # Grouping names are derived by the kernel, never published.
        self.assertIn("grouping", text)

    def test_manifests_declare_the_publications(self):
        expected = {
            "sio": ("/dev/serial/0", "SERIAL", 1, 0),
            "console": ("/dev/console", "CONSOLE", 6, 0),
            "nsdev": ("/dev/input/keyboard", "INPUT", 30, 1),
        }
        for name, (path, kind, cls, obj) in expected.items():
            data = json.loads(read(os.path.join(SERVICE_MANIFESTS,
                                                "%s.json" % name)))
            self.assertEqual(data["name"], name)
            self.assertEqual(len(data["publishes"]), len(PUBLICATIONS[name]))
            first = data["publishes"][0]
            self.assertEqual(first["path"], path)
            self.assertEqual(first["type"], kind)
            self.assertEqual(first["endpoint"]["class"], cls)
            self.assertEqual(first["object"], obj)

    def test_store_lists_the_six_images(self):
        data = json.loads(read(MANIFEST))
        self.assertEqual([entry["name"] for entry in data["files"]], STORE)

    def test_packed_containers_carry_the_table(self):
        """Decode the image's own containers: ABI 4, section counts and
        every publication row with its host-resolved endpoint number."""
        for name in SERVICE_ORDER:
            path = os.path.join(MODULE_DIR, "%s.zmod" % name)
            if not os.path.exists(path):
                self.skipTest("build-ns-selftest missing; run make "
                              "ns-selftest")
            with open(path, "rb") as handle:
                raw = handle.read()
            self.assertEqual(raw[0:4], b"ZMOD", name)
            self.assertEqual(raw[5], 2, name)     # ZMOD_TYPE_SERVICE
            self.assertEqual(raw[6], 4, name)     # ABI 4
            section = 28
            fields = struct.unpack_from("<HHHHBBBBHH", raw, section)
            (section_size, _seg, _ep, _dep, _sc, _ec, _dc, count, offset,
             reserved3) = fields
            self.assertEqual(reserved3, 0, name)
            self.assertLessEqual(section_size, len(raw) - section, name)
            rows = []
            for index in range(count):
                at = section + offset + index * 20
                label = raw[at:at + 16].split(b"\x00")[0].decode("ascii")
                kind, number, obj, flags = struct.unpack_from("<BBBB", raw,
                                                              at + 16)
                self.assertEqual(flags, 0, label)
                self.assertLessEqual(number, 3, label)
                rows.append((label, kind, number, obj))
            self.assertEqual(rows, PUBLICATIONS[name], name)

    def test_boot_manifest_matches_the_containers(self):
        """`services.man` is the loader's own boot manifest: one 24-byte
        record per image, in load order."""
        path = os.path.join(IMAGE_SUBDIR, "recovery.romfs")
        if not os.path.exists(path):
            self.skipTest("build-ns-selftest missing; run make ns-selftest")
        with open(path, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        names = [entry["name"] for entry in entries]
        self.assertEqual(names, STORE)
        manifest = next(e for e in entries if e["name"] == "services.man")
        data = manifest["payload"]
        self.assertEqual(len(data), MAN_RECORD * len(SERVICE_ORDER))
        for index, name in enumerate(STORE[:len(SERVICE_ORDER)]):
            record = data[index * MAN_RECORD:(index + 1) * MAN_RECORD]
            self.assertEqual(record.split(b"\x00")[0],
                             name.encode("ascii"))


class TestNsResidency(unittest.TestCase):
    def test_the_three_objects_are_cold_and_test_linked(self):
        tiers, cold_half, test_link = mapcheck.load_residency(RESIDENCY)
        for name in ("vfs_ns", "vfs_ns_file", "object"):
            self.assertEqual(tiers[name], "cold", name)
            self.assertIn(name, test_link)
        # Production's cold set is unchanged: the namespace objects reach
        # the image through its own --cold-extra, not through cold_half.
        for name in ("vfs_ns", "vfs_ns_file", "object"):
            self.assertNotIn(name, cold_half)

    def test_production_linker_script_omits_the_namespace(self):
        text = read(LINKER)
        for name in ("vfs_ns.rel", "vfs_ns_file.rel", "object.rel"):
            self.assertNotIn(name, text, name)
        # The M9 machinery it replaces is still linked, untouched.
        for name in ("fs.rel", "vfs_file.rel", "vfs_process.rel"):
            self.assertIn(name, text, name)

    def test_production_image_carries_no_namespace_symbol(self):
        map_path = os.path.join(PROD_SUBDIR, "zephyros.map")
        if not os.path.exists(map_path):
            self.skipTest("build/zephyros.map missing; run make all")
        found = symbols(map_path)
        # The M9 pair is what production dispatches: the fixed-core lookup
        # and call bodies, and the cold syscall front end.
        for name in ("_ipc_lookup", "_ipc_syscall_call", "_zos_ipc_dispatch"):
            self.assertIn(name, found, name)
        self.assertLess(found["_ipc_lookup"], SPLIT)
        self.assertLess(found["_ipc_syscall_call"], SPLIT)
        self.assertGreaterEqual(found["_zos_ipc_dispatch"], SPLIT)
        for name in ("_vfs_ns_bind", "_vfs_ns_file_bind",
                     "_zos_object_dispatch", "_vfs_ns_enum"):
            self.assertNotIn(name, found, name)


class TestNsClient(unittest.TestCase):
    def test_client_checks_the_documented_paths(self):
        text = read(CLIENT)
        for token in ('zos_ipc_bind("/SYSTEM/services.man"',
                      'zos_ipc_bind("/dev/console"',
                      'zos_ipc_bind("/dev/input/keyboard"',
                      'zos_ns_enum("/SYSTEM"',
                      "ns selftest: OK"):
            self.assertIn(token, text, token)
        # A root-level name has no provider owner: the router refuses it
        # instead of falling back to a provider.
        self.assertIn('check("file-root-refused"', text)
        self.assertIn('== -ZOS_ENOENT', text)
        self.assertIn("ZFILE_OP_READ", text)
        self.assertIn("ZFILE_OP_SEEK", text)
        self.assertIn("ZFILE_OP_META", text)

    def test_child_program_uses_the_standard_handles(self):
        text = read(CHILD)
        self.assertIn("ZOS_HANDLE_STDOUT", text)
        self.assertIn("zos_ipc_bind(\"/dev/console\"", text)
        self.assertIn("nschild: OK", text)

    def test_sdk_variant_exposes_the_family(self):
        header = read(SDK_HEADER)
        for token in ("int zos_ipc_bind(const char *path, unsigned int flags,"
                      " unsigned char *type);",
                      "int zos_ipc_unbind(unsigned char handle);",
                      "int zos_ns_enum(const char *path, unsigned int "
                      "*cookie,"):
            self.assertIn(token, header, token)
        source = read(SDK_SOURCE)
        self.assertIn("ZOS_NS_HANDLES", source)
        for token in ("ZOS_SYSCALL_BIND", "ZOS_SYSCALL_UNBIND",
                      "ZOS_SYSCALL_NS_ENUM", "zos_bind_block",
                      "zos_unbind_block", "zos_ns_enum_block"):
            self.assertIn(token, source, token)
        gate = read(SDK_GATE)
        # The three argument blocks the guarded marshalling writes: bind 8,
        # unbind 1, ns_enum 12 bytes, in the one writable SDK data area.
        self.assertIn(".area _ZOS_SDK_DATA", gate)
        self.assertIn("_zos_bind_block::\n\t.ds\t8", gate)
        self.assertIn("_zos_unbind_block::\n\t.ds\t1", gate)
        self.assertIn("_zos_ns_enum_block::\n\t.ds\t12", gate)
        zex_mk = read(ZEX_MK)
        self.assertIn("ZEX_DEFAULT_CODEBASE", zex_mk)
        self.assertIn("nsgate", zex_mk)


class TestNsImageLayout(unittest.TestCase):
    def setUp(self):
        self.map_path = os.path.join(IMAGE_SUBDIR, "zephyros.map")
        if not os.path.exists(self.map_path):
            self.skipTest("build-ns-selftest missing; run make ns-selftest")
        self.syms = symbols(self.map_path)

    def test_namespace_objects_link_cold(self):
        for name in ("_vfs_ns_bind", "_vfs_ns_unbind", "_vfs_ns_enum",
                     "_vfs_ns_file_bind", "_vfs_ns_file_dispatch",
                     "_zos_object_dispatch", "_vfs_ns_init"):
            address = self.syms[name]
            self.assertGreaterEqual(address, SPLIT, name)
            self.assertLess(address, 0xE000, name)
        # The stage-3 lookup body stays in the fixed core (it is linked but
        # unreachable: 06h routes to `bind` in this image).
        self.assertLess(self.syms["_ipc_lookup"], SPLIT)

    def test_the_acceptance_image_routes_the_family(self):
        """06h-09h reach the object dispatcher: the client's binds work
        only if the guarded family is the one the dispatcher calls."""
        source = read(os.path.join(ROOT, "kernel", "syscall.c"))
        self.assertIn("#if ZOS_NS_HANDLES", source)
        self.assertIn("zos_object_dispatch(zos_syscall_number", source)
        self.assertIn("_zos_object_dispatch", self.syms)
        self.assertIn("_vfs_ns_bind", self.syms)


if __name__ == "__main__":
    unittest.main()
