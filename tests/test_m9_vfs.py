"""Host tests for the M9 VFS bootstrap package.

Covers:
  - tools/mountgen.py accepts the production and tests/m9 manifests and
    rejects every section 32.5.1 case plus the extra rejection rules;
  - the generated C is deterministic and exactly the reviewed policy;
  - the kernel built-in provider table matches the Makefile's
    ZOS_VFS_BUILTIN list;
  - the M9 VFS object group links only into ROMFS-boot images, so the
    pre-M5 embedded-catalog images stay free of VFS bytes
    (correction-01);
  - include/zephyros/fs.h pins the M9 ABI values and structure sizes;
  - include/zephyros/vfs.h declares exactly the 14 path-based operations
    of architecture section 32.12 (correction-02).
"""

import importlib
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import mountgen  # noqa: E402

CONFIG = ROOT / "config" / "mounts.json"
M9_DIR = ROOT / "tests" / "m9"
MAKEFILE = ROOT / "Makefile"
MOUNT_C = ROOT / "kernel" / "vfs" / "mount.c"
FS_H = ROOT / "include" / "zephyros" / "fs.h"
VFS_H = ROOT / "include" / "zephyros" / "vfs.h"

SYSTEM = {"path": "/SYSTEM", "filesystem": "romfs", "source": "recovery",
          "system": True}


def manifest(mounts, fmt="vfs-mounts-v1"):
    return {"format": fmt, "mounts": mounts}


def minimal(**overrides):
    entry = dict(SYSTEM)
    entry.update(overrides)
    return manifest([entry])


class TestMountgenAccepts(unittest.TestCase):
    def test_production_manifest(self):
        entries = mountgen.validate(CONFIG, ["romfs"])
        self.assertEqual([e["path"] for e in entries], ["/SYSTEM"])
        self.assertEqual(entries[0]["flags"], mountgen.FLAG_SYSTEM)
        self.assertEqual(entries[0]["path_len"], 7)

    def test_every_tests_m9_manifest(self):
        manifests = sorted(M9_DIR.glob("mounts-*.json"))
        self.assertTrue(manifests)
        for path in manifests:
            entries = mountgen.validate(path, ["romfs"])
            self.assertEqual(sum(1 for e in entries if e["system"]), 1)

    def test_report_and_c_are_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp:
            out1 = os.path.join(tmp, "a.c")
            rep1 = os.path.join(tmp, "a.json")
            out2 = os.path.join(tmp, "b.c")
            rep2 = os.path.join(tmp, "b.json")
            self.assertEqual(mountgen.main([str(CONFIG), "--builtin", "romfs",
                                            "--out", out1, "--report", rep1]), 0)
            self.assertEqual(mountgen.main([str(CONFIG), "--builtin", "romfs",
                                            "--out", out2, "--report", rep2]), 0)
            self.assertEqual(Path(out1).read_bytes(), Path(out2).read_bytes())
            self.assertEqual(Path(rep1).read_bytes(), Path(rep2).read_bytes())
            report = json.loads(Path(rep1).read_text())
            self.assertEqual(report["count"], 1)
            self.assertEqual(report["mounts"][0]["path"], "/SYSTEM")
            self.assertEqual(report["mounts"][0]["flags"], "VFS_MOUNT_SYSTEM")
            self.assertEqual(report["builtin"], ["romfs"])

    def test_generated_c_shape(self):
        entries = mountgen.validate(CONFIG, ["romfs"])
        text = mountgen.render_c(entries)
        self.assertIn('#include "kernel/vfs/mount_config.h"', text)
        self.assertIn('const struct vfs_mount_config vfs_mount_table[] = {', text)
        self.assertIn('{ "/SYSTEM",', text)
        self.assertIn('"romfs",', text)
        self.assertIn('"recovery",', text)
        self.assertIn("VFS_MOUNT_SYSTEM", text)
        self.assertIn("const uint8_t vfs_mount_count = 1u;", text)

    def test_root_and_root_level_mounts_coexist(self):
        # `/` and a root-level mountpoint such as `/R2` are siblings in the
        # visible root, not nested, so the flat M9 model accepts them
        # together (architecture sections 32.6-32.7).
        man = manifest([
            dict(SYSTEM),
            {"path": "/", "filesystem": "ramfs", "source": "ramfs.0",
             "required": True},
            {"path": "/R2", "filesystem": "ramfs", "source": "ramfs.1",
             "required": False},
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "m.json")
            Path(path).write_text(json.dumps(man))
            entries = mountgen.validate(path, ["romfs"])
        self.assertEqual([e["path"] for e in entries],
                         ["/SYSTEM", "/", "/R2"])
        self.assertEqual(entries[1]["flags"], mountgen.FLAG_REQUIRED)
        self.assertEqual(entries[2]["flags"], mountgen.FLAG_OPTIONAL)

    def test_optional_and_required_flags(self):
        man = manifest([
            dict(SYSTEM),
            {"path": "/A", "filesystem": "romfs", "source": "x",
             "required": False},
            {"path": "/B", "filesystem": "romfs", "source": "x",
             "required": True},
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "m.json")
            Path(path).write_text(json.dumps(man))
            entries = mountgen.validate(path, ["romfs"])
        flags = {e["path"]: e["flags"] for e in entries}
        self.assertEqual(flags["/A"], mountgen.FLAG_OPTIONAL)
        self.assertEqual(flags["/B"], mountgen.FLAG_REQUIRED)


class TestMountgenRejects(unittest.TestCase):
    def reject(self, man):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "m.json")
            Path(path).write_text(json.dumps(man))
            with self.assertRaises(mountgen.MountError):
                mountgen.validate(path, ["romfs"])

    def test_unknown_format(self):
        self.reject(manifest([dict(SYSTEM)], fmt="vfs-mounts-v2"))

    def test_missing_mounts(self):
        self.reject({"format": "vfs-mounts-v1"})

    def test_empty_filesystem(self):
        self.reject(manifest([dict(SYSTEM, filesystem="")]))

    def test_empty_source(self):
        self.reject(manifest([dict(SYSTEM, source="")]))

    def test_zero_system(self):
        self.reject(manifest([{"path": "/A", "filesystem": "romfs",
                               "source": "x"}]))

    def test_two_system(self):
        self.reject(manifest([dict(SYSTEM),
                              dict(SYSTEM, path="/OTHER")]))

    def test_system_not_builtin(self):
        self.reject(manifest([dict(SYSTEM, filesystem="iocfat")]))

    def test_required_on_system(self):
        self.reject(manifest([dict(SYSTEM, required=True)]))

    def test_relative_path(self):
        self.reject(manifest([dict(SYSTEM, path="SYSTEM")]))

    def test_path_too_long(self):
        self.reject(manifest([dict(SYSTEM, path="/" + "A" * 128)]))

    def test_dot_components(self):
        self.reject(manifest([dict(SYSTEM, path="/./SYSTEM")]))
        self.reject(manifest([dict(SYSTEM, path="/../SYSTEM")]))

    def test_non_canonical_paths(self):
        self.reject(manifest([dict(SYSTEM, path="/System")]))
        self.reject(manifest([dict(SYSTEM, path="/SYSTEM/")]))
        self.reject(manifest([dict(SYSTEM, path="/SYS//TEM")]))

    def test_duplicate_mountpoints(self):
        self.reject(manifest([dict(SYSTEM),
                              {"path": "/SYSTEM", "filesystem": "romfs",
                               "source": "x"}]))

    def test_case_only_duplicate(self):
        self.reject(manifest([dict(SYSTEM),
                              {"path": "/system", "filesystem": "romfs",
                               "source": "x"}]))

    def test_duplicate_root(self):
        self.reject(manifest([
            {"path": "/", "filesystem": "romfs", "source": "x",
             "system": True},
            {"path": "/", "filesystem": "romfs", "source": "y"},
        ]))

    def test_nested_mountpoints(self):
        self.reject(manifest([
            dict(SYSTEM),
            {"path": "/SYSTEM/SUB", "filesystem": "romfs", "source": "x"},
        ]))

    def test_more_than_capacity(self):
        entries = [dict(SYSTEM)]
        for name in "ABCD":
            entries.append({"path": "/" + name, "filesystem": "romfs",
                            "source": "x"})
        self.reject(manifest(entries))

    def test_unknown_field(self):
        self.reject(manifest([dict(SYSTEM, extra=True)]))


class TestVfsObjectGroup(unittest.TestCase):
    """Every image links the one VFS object group.

    `mapcheck` enforces the linked result on the map (the kernel VFS core,
    the generated mount table, the ROMFS provider and the M9-only ROMFS
    accessors must all be bank-7 resident in every image); this check
    catches a re-addition of a conditional object list -- or of the
    retired boot-mode flags that selected one -- before any image is
    built.  See work/25a-retire-pre-m8-acceptance/WORK.md.
    """

    def setUp(self):
        self.text = MAKEFILE.read_text()

    def test_object_group_is_named_and_used_once(self):
        match = re.search(r"^M9_VFS_OBJS\s*:=\s*((?:.*\\\n)*.*)$",
                          self.text, re.M)
        self.assertIsNotNone(match, "Makefile has no M9_VFS_OBJS group")
        group = [path for path in re.split(r"[\s\\]+", match.group(0))
                 if path.endswith(".rel")]
        self.assertEqual(group, [
            "$(BUILD)/vfs_mounts.rel", "$(BUILD)/vfs_path.rel",
            "$(BUILD)/vfs_mount.rel", "$(BUILD)/vfs_file.rel",
            "$(BUILD)/vfs_dir.rel", "$(BUILD)/vfs_mutate.rel",
            "$(BUILD)/vfs_process.rel", "$(BUILD)/vfs_workspace.rel",
            "$(BUILD)/romfs_provider.rel"])
        # Join makefile continuations so a multi-line assignment counts
        # once; every group member is listed by the group only.
        logical = re.sub(r"\\\n\s*", " ", self.text).splitlines()
        assignments = [line for line in logical
                       if ":=" in line or "+=" in line]
        for obj in group:
            hits = [line for line in assignments if obj in line]
            self.assertEqual(len(hits), 1,
                             "%s must be listed in the M9_VFS_OBJS group "
                             "only (found %d assignments)" % (obj, len(hits)))
            self.assertIn("M9_VFS_OBJS :=", hits[0])
        self.assertIn("$(M9_VFS_OBJS)", self.text)

    def test_no_image_drops_the_group(self):
        # The embedded-catalog boot mode is retired: no filter-out, no
        # conditional append, and no boot-mode flag may select a
        # different VFS object set.
        self.assertNotIn("filter-out $(M9_VFS_OBJS)", self.text)
        for token in ("ZOS_ROMFS_BOOT", "ZOS_MONITOR_BOOT", "ZOS_M4_",
                      "module_embedded", "m4_catalog"):
            self.assertNotIn(token, self.text,
                             "Makefile still selects the retired %s path"
                             % token)

    def test_shared_sources_have_one_boot_path(self):
        # exec.c, romfs.c and main.c read and mount through the VFS
        # unconditionally; the compile-time selection is gone.
        for rel in ("kernel/process/exec.c", "kernel/romfs/romfs.c",
                    "kernel/main.c"):
            text = (ROOT / rel).read_text()
            self.assertNotIn("ZOS_ROMFS_BOOT", text,
                             "%s still selects a boot mode" % rel)
            self.assertNotIn("module_catalog_load_all", text,
                             "%s still boots from the embedded catalog" % rel)
        self.assertIn("kernel/vfs/vfs.h",
                      (ROOT / "kernel" / "process" / "exec.c").read_text(),
                      "exec.c must read its source through the VFS")


class TestBuiltinProviderParity(unittest.TestCase):
    def test_makefile_and_registry_agree(self):
        makefile = MAKEFILE.read_text()
        match = re.search(r"^ZOS_VFS_BUILTIN\s*\?=\s*(.+)$", makefile,
                          re.M)
        self.assertIsNotNone(match, "Makefile has no ZOS_VFS_BUILTIN")
        declared = [name for name in re.split(r"[,\s]+", match.group(1).strip())
                    if name]

        source = MOUNT_C.read_text()
        block = source.split("vfs_builtin_providers[] = {", 1)[1]
        block = block.split("};", 1)[0]
        registered = re.findall(r"&(\w+)_provider", block)
        self.assertEqual(sorted(declared), sorted(registered))


class TestProviderAbiShape(unittest.TestCase):
    """`include/zephyros/vfs.h` declares exactly the 14 path-based
    operations of architecture section 32.12, in that order, and nothing
    from the superseded node-based design (correction-02)."""

    # name -> parameter list, in architecture order.
    OPERATIONS = (
        ("mount", "void *provider_state, const char *source, void **fs"),
        ("open", "void *fs, const char *path, uint16_t flags, void **file,\n"
                 "\t\t    uint32_t *size"),
        ("close", "void *fs, void *file"),
        ("read", "void *fs, void *file, uint32_t offset, void *buf,\n"
                 "\t\t    uint16_t len"),
        ("write", "void *fs, void *file, uint32_t offset, const void *buf,\n"
                  "\t\t     uint16_t len"),
        ("sync", "void *fs, void *file"),
        ("stat", "void *fs, const char *path, struct zfs_stat *out"),
        ("opendir", "void *fs, const char *path, void **dir"),
        ("readdir", "void *fs, void *dir, struct zfs_dirent *out"),
        ("closedir", "void *fs, void *dir"),
        ("mkdir", "void *fs, const char *path"),
        ("rmdir", "void *fs, const char *path"),
        ("unlink", "void *fs, const char *path"),
        ("rename", "void *fs, const char *oldpath, const char *newpath"),
    )

    def setUp(self):
        self.text = VFS_H.read_text()
        self.ops = self.text.split("struct zfs_ops {", 1)[1].split("};", 1)[0]

    def test_exactly_fourteen_operations_in_order(self):
        found = re.findall(r"int \(\*(\w+)\)\(", self.ops)
        self.assertEqual(found, [name for name, _ in self.OPERATIONS])
        self.assertEqual(len(found), 14)

    def test_operation_signatures(self):
        def squash(text):
            return " ".join(text.split())

        found = {m.group(1): squash(m.group(2))
                 for m in re.finditer(r"int \(\*(\w+)\)\(([^)]*)\)",
                                      self.ops, re.S)}
        expected = {name: squash(params) for name, params in self.OPERATIONS}
        for name in expected:
            self.assertIn(name, found, "%s is missing" % name)
            self.assertEqual(found[name], expected[name],
                             "%s must keep the section 32.12 signature"
                             % name)

    def test_node_design_is_gone(self):
        for gone in ("zfs_open_info", "object_id", "lookup", "release",
                     "unmount", "syncfs", "truncate", "statfs"):
            self.assertNotIn(gone, self.ops,
                             "%s must not appear in the provider ABI" % gone)
        # Path-based means every path-taking operation takes a const char *.
        self.assertNotIn("void *parent", self.ops)
        self.assertNotIn("void *node", self.ops)

    def test_ops_count_matches(self):
        match = re.search(r"#define\s+ZFS_OPS_COUNT\s+(\d+)u", self.text)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), len(self.OPERATIONS))


class TestFsHeaderAbi(unittest.TestCase):
    def setUp(self):
        self.text = FS_H.read_text()

    def test_values(self):
        def has(name, value):
            pattern = r"#define\s+%s\s+%s\b" % (re.escape(name),
                                               re.escape(value))
            self.assertRegex(self.text, pattern)

        for name, value in (("ZOS_PATH_MAX", "128u"),
                            ("ZFS_NAME_MAX", "64u"),
                            ("ZFS_TYPE_FILE", "1u"), ("ZFS_TYPE_DIR", "2u"),
                            ("ZFS_ATTR_READONLY", "0x01u"),
                            ("ZFS_ATTR_HIDDEN", "0x02u"),
                            ("ZFS_ATTR_SYSTEM", "0x04u"),
                            ("ZFS_TIME_UNKNOWN", "0u"),
                            ("ZFS_O_READ", "0x01u"), ("ZFS_O_WRITE", "0x02u"),
                            ("ZFS_O_CREATE", "0x04u"), ("ZFS_O_EXCL", "0x08u"),
                            ("ZFS_O_TRUNC", "0x10u")):
            has(name, value)
        # M9 has no statfs/df (architecture section 32.12): a filesystem
        # -wide accounting type must not exist in the native ABI.
        self.assertNotIn("zfs_statfs", self.text)
        self.assertNotIn("ZFS_STATFS_READONLY", self.text)

    def test_structure_asserts(self):
        for assert_text in (
                "_Static_assert(sizeof(struct zfs_stat) == 16",
                "_Static_assert(sizeof(struct zfs_dirent) == 80",
                "__builtin_offsetof(struct zfs_stat, mtime) == 12",
                "__builtin_offsetof(struct zfs_dirent, name) == 16"):
            self.assertIn(assert_text, self.text)


if __name__ == "__main__":
    unittest.main()
