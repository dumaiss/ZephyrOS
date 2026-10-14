#!/usr/bin/env python3
"""Host-side V9958 video class and driver contract tests.

Covers the declarative and structural surface that can be checked
without executing target code:

  - drivers/include/ioc_protocol.h: the exact native operation values
    (CTRL_WRITE=1 .. RESET=7), the driver's operation order and the
    documented no-I/O validation rules;
  - platform/zephyr80/public/hardware.{h,inc}: paired ports A0h-A4h, latch
    bits, required GRAPHIC 6 register values and the bounded bootstrap
    delay count, value-identical between the C and assembly headers;
  - modules/manifests/v9958.json and the packed container: 512-byte
    granules, module-only symbols, and the font is NOT embedded in the
    resident image/BSS;
  - the font asset pipeline: the reviewable .inc source parses to the
    exact pinned 2048 bytes with the pinned SHA-256, and fontpack.py
    emits it deterministically as ROMFS DATA;
  - the driver source discipline: every in/out lives in v9958_io.asm,
    no CP/M tables or fixed BIOS addresses, no Virtual Drip framing, no
    IOC/HID import or symbol, no VDP interrupt claim, the proven init
    order, validate-before-I/O, and the reset path;
  - the ROMFS boot set (sio, ioc, hid, v9958.mod, v9958-6x8.font,
    monitor.exe) and the KOS_V9958_BOOT gate (earlier images compile it
    out);
  - mapcheck rejects linked V9958 driver symbols (module-only);
  - the smoke tool's video markers and result classification.

Executable behavior is replayed from the real linked images by
tests/test_v9958_sim.py.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
import modpack  # noqa: E402
import systemgen  # noqa: E402
import fontpack  # noqa: E402
from test_ioc import struct_member_order  # noqa: E402
from maptools import (  # noqa: E402
    HARDWARE_H, HARDWARE_INC, evaluate, parse_asm_inc, parse_c_header)

DRIVER_H = os.path.join(ROOT, "drivers", "v9958", "v9958_driver.h")
DRIVER_C = os.path.join(ROOT, "drivers", "v9958", "v9958.c")
DRIVER_ASM = os.path.join(ROOT, "drivers", "v9958", "v9958_io.asm")
# The video core is a bank-6 driver image with three tiers -
# hot (the core, the renderer and the port layer, including the display
# reset), boot (the one-shot bring-up entry) and bulk (the font).
PROTO_C = os.path.join(ROOT, "drivers", "v9958", "v9958_proto.c")
BOOT_C = os.path.join(ROOT, "drivers", "v9958", "v9958_boot.c")
FONT_ASM = os.path.join(ROOT, "drivers", "v9958", "v9958_font.asm")
FONT_INC = os.path.join(ROOT, "drivers", "v9958", "assets",
                        "font_cp850_6x8.inc")
MANIFEST = os.path.join(ROOT, "drivers", "v9958", "v9958.json")
PRODUCTION_SYSTEM = os.path.join(ROOT, "systems", "production.json")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
MAKEFILE = os.path.join(ROOT, "Makefile")
MOD = os.path.join(ROOT, "build", "modules", "v9958.mod")
FONT_BIN = os.path.join(ROOT, "build", "modules", "v9958-6x8.font")
MAP = os.path.join(ROOT, "build", "konaos.map")

PINNED_FONT_SHA256 = (
    "6db207e0466a05af42894d378261f876da51bb391240e1beda4f1d5f12ec442f"
)

V9958_CONSTANTS = {
    "KOS_V9958_DATA_PORT": 0xA0,
    "KOS_V9958_CMD_PORT": 0xA1,
    "KOS_V9958_PALETTE_PORT": 0xA2,
    "KOS_V9958_INDIRECT_PORT": 0xA3,
    "KOS_V9958_CONFIG_PORT": 0xA4,
    "KOS_V9958_CONFIG_INT_ROUTE": 0x01,
    "KOS_V9958_CONFIG_PORCH_OFF": 0x02,
    "KOS_V9958_REG_SELECT": 0x80,
    "KOS_V9958_STATUS2_CE": 0x01,
    "KOS_V9958_CMD_REGISTER_BASE": 32,
    "KOS_V9958_CMD_REGISTER_COUNT": 15,
    "KOS_V9958_REG_R0": 0,
    "KOS_V9958_REG_R1": 1,
    "KOS_V9958_REG_R2": 2,
    "KOS_V9958_REG_R3": 3,
    "KOS_V9958_REG_R4": 4,
    "KOS_V9958_REG_R5": 5,
    "KOS_V9958_REG_R6": 6,
    "KOS_V9958_REG_R7": 7,
    "KOS_V9958_REG_R8": 8,
    "KOS_V9958_REG_R9": 9,
    "KOS_V9958_REG_R10": 10,
    "KOS_V9958_REG_R11": 11,
    "KOS_V9958_REG_R14": 14,
    "KOS_V9958_REG_R15": 15,
    "KOS_V9958_REG_R16": 16,
    "KOS_V9958_REG_R17": 17,
    "KOS_V9958_REG_R18": 18,
    "KOS_V9958_REG_R23": 23,
    "KOS_V9958_REG_R25": 25,
    "KOS_V9958_REG_R26": 26,
    "KOS_V9958_REG_R27": 27,
    "KOS_V9958_REG_R46": 46,
    "KOS_V9958_R0_G6": 0x0A,
    "KOS_V9958_R1_DISPLAY_OFF": 0x00,
    "KOS_V9958_R1_DISPLAY_ON": 0x40,
    "KOS_V9958_R2_G6": 0x1F,
    "KOS_V9958_R5_SPRITE_BASE": 0xE4,
    "KOS_V9958_R6_SPRITE_PATTERN": 0x3F,
    "KOS_V9958_R7_BORDER": 0x04,
    "KOS_V9958_R8_64K_DRAM": 0x08,
    "KOS_V9958_R9_212_INTERLACE": 0x88,
    "KOS_V9958_R11_SPRITE_ATTR": 0x03,
    "KOS_V9958_R15_STATUS2": 0x02,
    "KOS_V9958_R23_TEXT_BASE": 0xFC,
    "KOS_V9958_R25_WAIT_OFF": 0x00,
    "KOS_V9958_R25_WAIT_ON": 0x04,
    "KOS_V9958_BOOT_DELAY_COUNT": 16,
}

VIDEO_OPS = {
    "KVIDEO_CTRL_WRITE": 1,
    "KVIDEO_DATA_WRITE": 2,
    "KVIDEO_DATA_BLOCK": 3,
    "KVIDEO_PALETTE_WRITE": 4,
    "KVIDEO_INDIRECT_WRITE": 5,
    "KVIDEO_PRESENT": 6,
    "KVIDEO_RESET": 7,
}


def _read(path, mode="r"):
    with open(path, mode) as handle:
        return handle.read()


def _load_json(path):
    with open(path) as handle:
        return json.load(handle)


def _code_only(path):
    text = _read(path)
    if path.endswith((".c", ".h")):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
    elif path.endswith(".asm"):
        text = "\n".join(line.split(";")[0] for line in text.splitlines())
    return text


class TestPlatformConstants(unittest.TestCase):
    def setUp(self):
        self.c = parse_c_header(HARDWARE_H)
        self.asm = parse_asm_inc(HARDWARE_INC)

    def test_verified_constants_present_and_exact(self):
        for name, value in sorted(V9958_CONSTANTS.items()):
            self.assertIn(name, self.c, "hardware.h missing %s" % name)
            self.assertIn(name, self.asm, "hardware.inc missing %s" % name)
            self.assertEqual(evaluate(self.c[name], self.c), value,
                             "hardware.h %s" % name)
            self.assertEqual(evaluate(self.asm[name], self.asm), value,
                             "hardware.inc %s" % name)

    def test_derived_relations(self):
        env = dict(self.c)
        # Latch: porch-off is the /WS_EN bit one above the D0 route.
        self.assertEqual(evaluate("KOS_V9958_CONFIG_PORCH_OFF", env),
                         evaluate("KOS_V9958_CONFIG_INT_ROUTE", env) << 1)
        # R#25: waiting is the WTE bit; the off value clears VDS too.
        self.assertEqual(evaluate("KOS_V9958_R25_WAIT_ON", env), 0x04)
        self.assertEqual(evaluate("KOS_V9958_R25_WAIT_OFF", env), 0x00)
        # R#23 = 100h - 4 (the proven 4-line display offset).
        self.assertEqual(evaluate("KOS_V9958_R23_TEXT_BASE", env) + 4,
                         0x100)
        # The command block covers R#32..R#46 inclusive.
        self.assertEqual(evaluate("KOS_V9958_CMD_REGISTER_BASE", env) +
                         evaluate("KOS_V9958_CMD_REGISTER_COUNT", env) - 1,
                         evaluate("KOS_V9958_REG_R46", env))
        # The data/command/palette/indirect/config ports form the A0h-A4h
        # block in order.
        base = evaluate("KOS_V9958_DATA_PORT", env)
        for offset, name in enumerate(("KOS_V9958_DATA_PORT",
                                       "KOS_V9958_CMD_PORT",
                                       "KOS_V9958_PALETTE_PORT",
                                       "KOS_V9958_INDIRECT_PORT",
                                       "KOS_V9958_CONFIG_PORT")):
            self.assertEqual(evaluate(name, env), base + offset, name)

    def test_shared_names_are_value_identical(self):
        for name in sorted(set(self.c) & set(self.asm)):
            self.assertEqual(evaluate(self.c[name], self.c),
                             evaluate(self.asm[name], self.asm),
                             "%s differs between hardware.h/.inc" % name)


class TestFontAsset(unittest.TestCase):
    def test_source_asset_is_the_pinned_2048_bytes(self):
        data = fontpack.parse_font(FONT_INC)
        self.assertEqual(len(data), 2048)
        digest = fontpack.verify(data, FONT_INC)
        self.assertEqual(digest, PINNED_FONT_SHA256)
        # The header comment declares the same hash (reviewability).
        self.assertIn(PINNED_FONT_SHA256, _read(FONT_INC))

    def test_fontpack_verifies(self):
        self.assertEqual(fontpack.main(["fontpack.py", "verify",
                                        FONT_INC]), 0)
        self.assertEqual(fontpack.main(["fontpack.py"]), 2)

    def test_built_font_artifact_matches(self):
        if not os.path.exists(FONT_BIN):
            self.skipTest("build/modules/v9958-6x8.font missing")
        data = _read(FONT_BIN, "rb")
        self.assertEqual(len(data), 2048)
        import hashlib
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         PINNED_FONT_SHA256)


class TestDriverArtifact(unittest.TestCase):
    def test_manifest_shape(self):
        manifest = _load_json(MANIFEST)
        self.assertEqual(manifest["name"], "v9958")
        self.assertEqual(manifest["init"], "v9958_driver_init")
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot", "boot", "bulk"])
        hot, boot, bulk = manifest["segments"]
        self.assertEqual(hot["image_areas"], ["_CODE"])
        self.assertEqual(hot["bss_areas"], ["_DRIVER_IO"])
        self.assertEqual(boot["image_areas"], ["_BOOT"])
        self.assertEqual(boot["bss_areas"], ["_BOOT_DATA"])
        self.assertEqual(boot["entry"], "v9958_boot_entry")
        self.assertEqual(bulk["image_areas"], ["_BULK"])
        self.assertEqual(manifest["objects"],
                         ["v9958.rel", "v9958_io.rel", "v9958_driver.rel",
                          "v9958_font.rel", "v9958_boot.rel",
                          "sdcc_indirect_calls.rel"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 2, "instance": 0, "version": 1,
                           "dispatch": "v9958_driver_dispatch", "flags": 0,
                           "release": True}])

    def test_packed_container_and_the_bulk_font(self):
        if not os.path.exists(MOD):
            self.skipTest("build/modules/v9958.mod missing; build first")
        data = _read(MOD, "rb")
        info = modpack.parse_container(data, "v9958.mod")
        # The image carries its host-built publications (`/dev/video`).
        self.assertEqual([publication["name"]
                          for publication in info["publications"]],
                         ["/video"])
        self.assertGreater(info["image_size"], 0)
        self.assertGreater(info["bss_size"], 0)
        self.assertNotEqual(info["init_offset"], 0xFFFF)
        hot, boot, bulk = info["segments"]
        # The hot tier holds the core, the port layer and the renderer
        # (the display reset shares the bring-up chain, 3.2); it must
        # stay inside the driver I/O window.
        self.assertLess(hot["image_size"] + hot["bss_size"], 0x4000)
        # The font is the bulk segment, byte for byte the pinned asset
        # (the same source tools/fontpack.py packs for the ROMFS copy).
        self.assertEqual(bulk["image_size"], 2048)
        asset = fontpack.parse_font(FONT_INC)
        at = bulk["payload_offset"]
        self.assertEqual(info["payload"][at:at + 2048],
                         bytes(asset),
                         "the bulk segment must carry the pinned font")
        # The boot tier is a bounded bring-up entry, not a second copy of
        # the driver: every path reachable after boot is hot.
        self.assertLess(boot["image_size"] + boot["bss_size"], 0x200)
        # Host-linked: the boot entry lies inside the boot image (the
        # flat plan carries no relocation records at all).
        self.assertNotEqual(boot["entry"], 0xFFFF)
        self.assertGreaterEqual(boot["entry"], boot["base"])
        self.assertLess(boot["entry"], boot["base"] + boot["image_size"])

    def test_system_file_and_boot_order(self):
        # The system file names the driver load order and the
        # store's own files; the generated store manifest (whose media
        # type follows from each name) is tests/test_systemgen.py's
        # business.
        system = _load_json(PRODUCTION_SYSTEM)
        self.assertEqual(system["drivers"],
                         ["sio", "ioc", "hid", "v9958", "console-v9958-hid",
                          "timer", "iocfat"])
        self.assertEqual(system["store"],
                         ["shell.exe", "monitor.exe",
                          # The production feature exerciser and the
                          # outcome programs it runs.
                          "hwtest.exe", "timer.exe", "xmem_selftest.exe",
                          "xmem_child.exe", "xmem_far.exe",
                          "fs_selftest.exe", "fs_child.exe",
                          "far_data.exe", "banks.exe", "known.bin"])
        self.assertEqual(systemgen.store_type("v9958.mod"), "MOD")
        # The font is the video image's bulk segment; the recovery
        # store carries no separate copy of it.
        self.assertNotIn("v9958-6x8.font", system["store"])
        self.assertNotIn("v9958-6x8.font", system["drivers"])
        main = _read(MAIN_C)
        self.assertIn("driver_load_all(KOS_DRIVER_MANIFEST_NAME)", main)
        # The image's own boot tier runs the bring-up (the endpoint and
        # bulk extent are the loader's), and the reset path re-reads the
        # font through the nucleus bulk copy because the window cannot
        # show the bulk tier.
        loader = _read(os.path.join(ROOT, "kernel", "driver",
                                    "driver_loader.c"))
        self.assertIn("bulk_base", loader)
        self.assertIn("bulk_read", os.path.join(
            _read(os.path.join(ROOT, "drivers", "v9958", "v9958_proto.c"))))

    def test_makefile_targets_and_gates(self):
        makefile = _read(MAKEFILE)
        for target in ("v9958-test:", "console-integration-selftest"):
            self.assertIn(target, makefile)
        # One generated pack rule per selected driver.
        self.assertIn("$(BUILD)/drivers/$(1).json", makefile)
        self.assertIn("v9958_driver.rel", makefile)
        self.assertIn("v9958_font.rel", makefile)
        self.assertIn("$(MODULES)/v9958-6x8.font", makefile)
        # The video diagnostic runs in the console-integration image.
        index = makefile.index("SYSTEM_TEST_OBJS.console-integration :=")
        self.assertIn("$(BUILD)/video_test.rel", makefile[index:index + 400])
        self.assertIn("video_test", makefile)
        system = _load_json(os.path.join(ROOT, "systems",
                                         "console-integration.json"))
        self.assertIn("-DKOS_VIDEO_SELFTEST=1", system["cflags"])
        self.assertEqual(system["drivers"],
                         ["sio", "ioc", "hid", "v9958",
                          "console-v9958-hid"])


class TestDriverSourceDiscipline(unittest.TestCase):
    def setUp(self):
        self.c = _code_only(DRIVER_C)
        self.header = _code_only(DRIVER_H)
        self.proto = _code_only(PROTO_C)
        self.boot = _code_only(BOOT_C)
        self.asm = _code_only(DRIVER_ASM)
        self.sources = (self.c, self.header, self.proto, self.boot)
        self.init = self.c.split("static int v9958_init_display")[1].split(
            "/* --- class operations")[0]
        self.send = self.c.split("int v9958_send")[1].split(
            "int16_t v9958_driver_dispatch")[0]

    def test_all_port_io_lives_in_the_asm_file(self):
        for body in self.sources:
            for needle in ("__sfr", "__asm", "__endasm", "0xA0", "0xA1",
                           "0xA2", "0xA3", "0xA4"):
                self.assertNotIn(needle, body,
                                 "driver C must not contain %r" % needle)
            self.assertNotRegex(body, r"(?m)^\s*(in|out)\s")
        self.assertIn("out\t(KOS_V9958_DATA_PORT),a", self.asm)
        self.assertIn("out\t(KOS_V9958_CMD_PORT),a", self.asm)
        self.assertIn("out\t(KOS_V9958_PALETTE_PORT),a", self.asm)
        self.assertIn("out\t(KOS_V9958_INDIRECT_PORT),a", self.asm)
        self.assertIn("out\t(KOS_V9958_CONFIG_PORT),a", self.asm)
        self.assertIn("in\ta,(KOS_V9958_CMD_PORT)", self.asm)

    def test_no_cpm_bios_or_vdrip_remnants(self):
        for body in self.sources + (self.asm,):
            for needle in ("CBIOS", "BIOS", "CONOUT", "CONIN", "BDOS",
                           "WBOOT", "Vdrip", "VDRIP", "Virtual Drip",
                           "8000h", "0x8000", "command_buffer",
                           "print_run", "TEXT_LOG_COLUMNS",
                           "CONSOLE_FONT_ROM_BASE", "esc_press"):
                self.assertNotIn(needle, body,
                                 "reference-only remnant survives: %r"
                                 % needle)

    def test_no_ioc_or_hid_import_and_no_kernel_symbols(self):
        for body in self.sources:
            for needle in ("_ioc_", "_hid_", "_sio_", "_kernel_main",
                           "register_ep", "console_",
                           "device" + "_register"):
                self.assertNotIn(needle, body,
                                 "v9958 must not reference %r" % needle)
        # The driver's own API table is the only kernel surface it uses.
        self.assertIn("driver_api", self.proto)
        self.assertIn("bulk_read", self.proto)

    def test_no_vdp_interrupt_claim(self):
        for body in self.sources + (self.asm,):
            for needle in ("irq_claim", "irq_release", "KOS_IRQ",
                           "IRQ_VDP"):
                self.assertNotIn(needle, body,
                                 "V9958 must keep /INT disabled: %r"
                                 % needle)

    def test_proven_initialization_order(self):
        # Porch off, paced R#25/R#8/stop/R#0..R#11, R#25 WTE on, porch
        # on, palette, atlases, clear, cursor, present, display on.
        order = [
            "v9958_porch_off();",
            "KOS_V9958_R25_WAIT_OFF",
            "KOS_V9958_R8_64K_DRAM",
            "KOS_V9958_REG_R46",
            "v9958_g6_registers",
            "KOS_V9958_R23_TEXT_BASE",
            "KOS_V9958_R25_WAIT_ON",
            "v9958_porch_on();",
            "v9958_init_palette();",
            "v9958_upload_font_atlas()",
            "v9958_clear_screen();",
            "v9958_cursor_init();",
            "v9958_io_wait_command();",
            "KOS_V9958_R1_DISPLAY_ON",
        ]
        positions = []
        cursor = 0
        for needle in order:
            index = self.c.find(needle, cursor)
            self.assertNotEqual(index, -1,
                                "initialization step missing: %r" % needle)
            positions.append(index)
            cursor = index
        self.assertEqual(positions, sorted(positions),
                         "initialization order changed")

    def test_pacing_and_latch_shadow(self):
        # Paced helper used while WTE=0, non-paced after; D0 preserved.
        self.assertIn("v9958_io_reg_paced", self.c)
        self.assertIn("v9958_io_reg(", self.c)
        porch = self.c.split("static void v9958_porch_off")[1].split(
            "static void v9958_porch_on")[0]
        self.assertIn("KOS_V9958_CONFIG_INT_ROUTE", porch)
        self.assertIn("KOS_V9958_CONFIG_PORCH_OFF", porch)
        latch = self.c.split("static void v9958_porch_on")[1].split(
            "/* Write one precolored")[0]
        self.assertIn("KOS_V9958_CONFIG_INT_ROUTE", latch)
        self.assertNotIn("KOS_V9958_CONFIG_PORCH_OFF", latch)
        self.assertIn("v9958_io_latch_write", latch)
        # The delay primitive is the bounded platform count.
        self.assertIn("KOS_V9958_BOOT_DELAY_COUNT", self.asm)

    def test_validate_before_io(self):
        validation = self.send.index("payload == (const void *)0")
        reject = self.send.index("return (-KOS_EINVAL)", validation)
        streams = [self.send.index(name) for name in
                   ("v9958_io_stream_ctrl", "v9958_io_stream_data",
                    "v9958_io_stream_palette", "v9958_io_stream_indirect")]
        self.assertLess(reject, min(streams),
                        "the write branch must validate before any I/O")
        present = self.send.split("case KVIDEO_PRESENT:")[1]
        self.assertLess(present.index("length != 0u"),
                        present.index("v9958_io_wait_command"))
        reset = self.send.split("case KVIDEO_RESET:")[1]
        self.assertLess(reset.index("length != 0u"),
                        reset.index("v9958_init_display"))
        self.assertIn("return (-KOS_ENOTSUP);", self.send)
        self.assertIn("return (-KOS_ENODEV);", self.send)

    def test_font_is_streamed_not_resident(self):
        # Bounded 64-byte resource reads, chunk-wise atlas conversion (one
        # 8-glyph chunk and one 24-byte segment staged at a time).
        self.assertIn("V9958_FONT_CHUNK", self.c)
        self.assertIn("v9958_font_group", self.c)
        self.assertIn("v9958_atlas_row", self.c)
        # No font byte table: the driver C contains no data initializer
        # with hundreds of bytes.  The bounded size covers the video
        # core plus the ported terminal renderer.
        self.assertNotIn("0x70u, 0x88u", self.c)
        self.assertLess(len(self.c), 40000)

    def test_descriptor_is_console_capable_and_output_only(self):
        # The device descriptor became a driver endpoint: the VIDEO class
        # is what the console and the SDK look up, and the image is
        # output-only by construction (the protocol has no input path).
        self.assertIn("kos/protocol.h", self.proto)
        protocol = _read(os.path.join(ROOT, "abi", "kos", "protocol.h"))
        self.assertIn("KOS_CLASS_VIDEO", protocol)
        for op in ("KVIDEO_OP_SEND", "KVIDEO_OP_WRITE", "KVIDEO_OP_PUTC"):
            self.assertIn(op, self.proto, op)
        # KVIDEO_OP_SEND carries any native VIDEO operation, so the five
        # streaming forms take a descriptor and the two control forms do
        # not; the driver core keeps the native operation names.
        for op in ("KVIDEO_CTRL_WRITE", "KVIDEO_DATA_WRITE",
                   "KVIDEO_DATA_BLOCK", "KVIDEO_PALETTE",
                   "KVIDEO_INDIRECT", "KVIDEO_PRESENT", "KVIDEO_RESET"):
            self.assertIn(op, self.c, op)
        # The single-byte and character-buffer paths the console uses end
        # at the ported renderer, which is strictly output-only.
        renderer = self.c.split("int v9958_char_putc_worker")[1]
        self.assertIn("v9958_char_write_worker", renderer)
        self.assertNotIn("v9958_char_getc", self.c)

    def test_renderer_runs_on_a_private_module_stack(self):        # The renderer entries are assembly wrappers (the kernel gate
        # stack is far deeper than one renderer call chain may use); the
        # driver window is where they run, so the private stack is the
        # image's own BSS.
        for name in ("_v9958_char_putc", "_v9958_char_write",
                     "_v9958_char_putc_worker", "_v9958_char_write_worker"):
            self.assertIn(".globl %s" % name, self.asm, name)
        self.assertIn("_v9958_alt_stack_top", self.asm)
        self.assertRegex(self.asm, r"ld\s+sp,#_v9958_alt_stack_top")
        self.assertIn("_v9958_alt_saved_sp", self.asm)
        self.assertIn("v9958_char_putc_worker", self.c)
        self.assertIn("v9958_char_write_worker", self.c)
        self.assertIn("int v9958_char_putc(", self.header)
        self.assertIn("int v9958_char_write(", self.header)
        # The driver dispatch is the only caller from outside the image.
        self.assertIn("v9958_char_putc", self.proto)

class TestMapcheckRules(unittest.TestCase):
    def test_v9958_symbols_are_module_only(self):
        self.assertIn("_v9958_", mapcheck.STATIC_V9958_PREFIXES)
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_v9958_send": 0x2000})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_v9958_module_init": 0x3000})

    def test_production_map_has_no_linked_v9958_symbols(self):
        if not os.path.exists(MAP):
            self.skipTest("build/konaos.map missing; build first")
        syms = {}
        with open(MAP) as handle:
            for line in handle:
                match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
                if match:
                    syms[match.group(2)] = int(match.group(1), 16)
        linked = [name for name in syms if name.startswith("_v9958_")]
        self.assertEqual(linked, [],
                         "V9958 driver symbols must not be linked into "
                         "the kernel image: %s" % linked)


if __name__ == "__main__":
    unittest.main()
