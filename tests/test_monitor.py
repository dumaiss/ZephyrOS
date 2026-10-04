#!/usr/bin/env python3
"""Host-side M7 monitor tests: ABI/tool constant parity, ZEX layout,
source/map dependency inspection, and artifact integrity.

Coverage:

* diagnostic syscall numbers and packed-block constants are value-identical
  across include/zephyros/syscall.h, user/runtime/zex_runtime.h, and the
  assembly wrappers in user/runtime/zex_diag_runtime.asm;
* the monitor manifest/rights: ordinary ZEX v1, entry at 0100h, exact
  linked image size, stack reservation, and a low-window scratch block;
* the linked monitor map contains only monitor/runtime modules - no
  kernel-private symbols, no SIO/early-console/CP/M dependencies - and
  the packed monitor.zex is stored byte-identically in the M7 and
  production ROMFS images;
* the monitor and its runtime sources contain no direct port I/O, SIO
  port constants, early-console calls, CP/M vectors/BDOS calls, or
  private kernel symbol references;
* sample text of the monitor command surface is present in the image.

Skipped when the packed monitor / images have not been built.
"""

import contextlib
import hashlib
import io
import json
import os
import re
import sys
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mapcheck  # noqa: E402
import romfspack  # noqa: E402
import zexpack  # noqa: E402
from test_m1 import evaluate, make_map, parse_c_header  # noqa: E402

SYSCALL_H = os.path.join(ROOT, "include", "zephyros", "syscall.h")
RUNTIME_H = os.path.join(ROOT, "user", "runtime", "zex_runtime.h")
RUNTIME_ASM = os.path.join(ROOT, "user", "runtime", "zex_diag_runtime.asm")
CONSOLE_H = os.path.join(ROOT, "user", "runtime", "console_io.h")
MONITOR_JSON = os.path.join(ROOT, "user", "monitor", "monitor.json")
M7_MANIFEST = os.path.join(ROOT, "tests", "m7", "manifest.json")
PROD_MANIFEST = os.path.join(ROOT, "recovery", "manifest.json")

MONITOR_SOURCES = (
    os.path.join(ROOT, "user", "monitor", "monitor.c"),
    os.path.join(ROOT, "user", "monitor", "monitor.h"),
    os.path.join(ROOT, "user", "monitor", "monitor_mem.c"),
    os.path.join(ROOT, "user", "monitor", "monitor_io.c"),
    os.path.join(ROOT, "user", "monitor", "monitor_entry.asm"),
)

# The complete module set of the linked monitor (proves nothing from the
# kernel was dragged into the ZEX link).
MONITOR_MODULES = {
    "monitor_entry", "monitor", "monitor_mem", "monitor_io",
    "console_io", "hexfmt", "hexdump", "status",
    "zex_runtime", "zex_diag_runtime",
}

# Private kernel symbols that must never appear in a user ZEX.  The
# runtime's own `_zos_runtime_scratch` is a user-side object (M6), so the
# sweep names the kernel families instead of the bare `_zos_` prefix.
KERNEL_PREFIXES = (
    "_zos_syscall", "_zos_diag", "_zos_bank", "_zos_user", "_zos_process",
    "_process_", "_kernel", "_irq_", "_console_", "_device_",
    "_romfs_", "_module_", "_zmod_", "_zex_validate", "_early_console",
    "_panic", "_platform_", "_rom_access", "irq_", "romfs_",
)


def read_text(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def parse_asm_constants(text):
    values = {}
    for match in re.finditer(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*$",
                             text, re.M):
        values[match.group(1)] = int(match.group(2), 0)
    return values


class TestDiagConstantParity(unittest.TestCase):
    def setUp(self):
        self.syscall = parse_c_header(SYSCALL_H)
        self.runtime = parse_c_header(RUNTIME_H)
        self.asm = parse_asm_constants(read_text(RUNTIME_ASM))

    def _sys(self, name):
        return evaluate(self.syscall[name], self.syscall)

    def _rt(self, name):
        return evaluate(self.runtime[name], self.runtime)

    def test_syscall_numbers_match_the_assembly_wrappers(self):
        expected = {
            "SYS_DIAG_MEM_READ": self._sys("ZOS_SYSCALL_DIAG_MEM_READ"),
            "SYS_DIAG_MEM_WRITE": self._sys("ZOS_SYSCALL_DIAG_MEM_WRITE"),
            "SYS_DIAG_IO_IN": self._sys("ZOS_SYSCALL_DIAG_IO_IN"),
            "SYS_DIAG_IO_OUT": self._sys("ZOS_SYSCALL_DIAG_IO_OUT"),
            "SYS_DIAG_BANK_INFO": self._sys("ZOS_SYSCALL_DIAG_BANK_INFO"),
            "SYS_DIAG_SYSTEM_INFO": self._sys("ZOS_SYSCALL_DIAG_SYSTEM_INFO"),
        }
        self.assertEqual(expected, {
            "SYS_DIAG_MEM_READ": 0x28,
            "SYS_DIAG_MEM_WRITE": 0x29,
            "SYS_DIAG_IO_IN": 0x2A,
            "SYS_DIAG_IO_OUT": 0x2B,
            "SYS_DIAG_BANK_INFO": 0x2C,
            "SYS_DIAG_SYSTEM_INFO": 0x2D,
        })
        for name, value in expected.items():
            self.assertEqual(self.asm.get(name), value,
                             "%s differs between syscall.h and the "
                             "assembly wrapper" % name)

    def test_range_and_mask_constants_match(self):
        pairs = (
            ("ZOS_DIAG_MEM_BANK_MAX", "ZEX_DIAG_MEM_BANK_MAX"),
            ("ZOS_DIAG_MEM_READ_MIN", "ZEX_DIAG_MEM_READ_MIN"),
            ("ZOS_DIAG_MEM_READ_MAX", "ZEX_DIAG_MEM_READ_MAX"),
            ("ZOS_DIAG_MEM_WRITE_MIN", "ZEX_DIAG_MEM_WRITE_MIN"),
            ("ZOS_DIAG_MEM_WRITE_MAX", "ZEX_DIAG_MEM_WRITE_MAX"),
            ("ZOS_DIAG_IO_PORT_MIN", "ZEX_DIAG_IO_PORT_MIN"),
            ("ZOS_DIAG_IO_PORT_MAX", "ZEX_DIAG_IO_PORT_MAX"),
            ("ZOS_DIAG_BANK_MASK", "ZEX_DIAG_BANK_MASK"),
            ("ZOS_DIAG_SYSTEM_INFO_SIZE", "ZEX_DIAG_SYSTEM_INFO_SIZE"),
            ("ZOS_DIAG_SYSTEM_INFO_ABI", "ZEX_DIAG_SYSTEM_INFO_ABI"),
        )
        for kernel_name, user_name in pairs:
            self.assertEqual(self._sys(kernel_name), self._rt(user_name),
                             "%s/%s differ" % (kernel_name, user_name))
        self.assertEqual(self._sys("ZOS_DIAG_MEM_BANK_MAX"), 6)
        self.assertEqual(self._sys("ZOS_DIAG_MEM_READ_MIN"), 0x0000)
        self.assertEqual(self._sys("ZOS_DIAG_MEM_READ_MAX"), 0xDFFF)
        self.assertEqual(self._sys("ZOS_DIAG_MEM_WRITE_MIN"), 0x0100)
        self.assertEqual(self._sys("ZOS_DIAG_MEM_WRITE_MAX"), 0xDFFF)
        self.assertEqual(self._sys("ZOS_DIAG_IO_PORT_MIN"), 0x10)
        self.assertEqual(self._sys("ZOS_DIAG_BANK_MASK"), 0x7F)
        self.assertEqual(self._sys("ZOS_DIAG_SYSTEM_INFO_SIZE"), 8)

    def test_packed_block_asserts_present(self):
        text = re.sub(r"\s+", " ", read_text(SYSCALL_H))
        for statement in (
                "sizeof(struct zos_diag_mem_read_args) == 4",
                "sizeof(struct zos_diag_mem_write_args) == 4",
                "sizeof(struct zos_diag_io_in_args) == 1",
                "sizeof(struct zos_diag_io_out_args) == 2",
                "sizeof(struct zos_diag_system_info) == "
                "ZOS_DIAG_SYSTEM_INFO_SIZE",
                "__builtin_offsetof(struct zos_diag_system_info, "
                "common_base) == 4",
                "__builtin_offsetof(struct zos_diag_system_info, "
                "kernel_bank) == 6",
                "__builtin_offsetof(struct zos_diag_system_info, "
                "romfs_first_page) == 7"):
            self.assertIn(statement, text,
                          "packed-layout assertion missing: %s" % statement)

    def test_line_limit_is_80_characters(self):
        console = parse_c_header(CONSOLE_H)
        self.assertEqual(evaluate(console["ZRT_LINE_MAX"], console), 80)
        monitor_h = parse_c_header(
            os.path.join(ROOT, "user", "monitor", "monitor.h"))
        env = dict(console)
        env.update(monitor_h)
        self.assertEqual(evaluate(env["MON_LINE_MAX"], env), 81)


class TestMonitorArtifacts(unittest.TestCase):
    def _read(self, rel):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            self.skipTest("%s missing; build it first" % rel)
        with open(path, "rb") as fh:
            return path, fh.read()

    def test_manifest_declares_ordinary_zex_with_low_scratch(self):
        manifest = zexpack.load_manifest(MONITOR_JSON)
        self.assertEqual(manifest["entry"], "mon_entry")
        self.assertEqual(manifest["entry_offset"], 0)
        self.assertEqual(manifest["scratch"]["symbol"], "zex_diag_scratch")
        self.assertEqual(manifest["scratch"]["size"], 12)

    def test_packed_monitor_matches_the_manifest_and_report(self):
        _p, data = self._read("build/modules/monitor.zex")
        _p, report_text = self._read("build/modules/monitor.report.json")
        report = json.loads(report_text)
        manifest = zexpack.load_manifest(MONITOR_JSON)
        fields, geo = zexpack.validate_image(data)
        self.assertEqual(fields["image_size"], manifest["image_size"])
        self.assertEqual(fields["bss_size"], manifest["bss_size"])
        self.assertEqual(fields["stack_size"], manifest["stack_size"])
        self.assertEqual(geo["entry"], 0x0100)
        self.assertEqual(report["sha256"],
                         hashlib.sha256(data).hexdigest())
        self.assertEqual(report["scratch"], manifest["scratch"])
        self.assertEqual(report["entry"]["symbol"], "mon_entry")

    def test_linked_map_contains_only_monitor_and_runtime_modules(self):
        _p, raw = self._read("build/monitor/monitor.map")
        text = raw.decode("utf-8", "replace")
        modules = set()
        symbols = {}
        for line in text.splitlines():
            match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)\s+(\S+)\s*$",
                             line)
            if match:
                symbols[match.group(2)] = int(match.group(1), 16)
                modules.add(match.group(3))
        self.assertEqual(modules, MONITOR_MODULES,
                         "monitor link pulled in unexpected modules")
        for name in symbols:
            for prefix in KERNEL_PREFIXES:
                self.assertFalse(
                    name.startswith(prefix),
                    "linked monitor map references private symbol %s"
                    % name)
        # The runtime's own scratch symbols stay inside the caller window.
        for name in ("zos_runtime_scratch", "zex_diag_scratch"):
            address = symbols[name]
            self.assertGreaterEqual(address, 0x0100)
            self.assertLess(address, 0x2000)

    def test_romfs_images_store_the_same_monitor_bytes(self):
        _p, packed = self._read("build/modules/monitor.zex")
        for image_dir in ("build-m7-selftest", "build"):
            rel = os.path.join(image_dir, "recovery.romfs")
            _p, stream = self._read(rel)
            _h, entries = romfspack.parse_image(stream)
            names = {entry["name"]: entry for entry in entries}
            self.assertIn("monitor.zex", names,
                          "%s does not carry monitor.zex" % rel)
            self.assertEqual(names["monitor.zex"]["flags"],
                             romfspack.FLAG_ZEX)
            self.assertEqual(names["monitor.zex"]["payload"], packed,
                             "%s monitor.zex payload differs from the "
                             "packed artifact" % rel)

    def test_manifests_name_monitor_as_a_zex(self):
        m7 = json.loads(read_text(M7_MANIFEST))
        entries = {entry["name"]: entry for entry in m7["files"]}
        self.assertEqual(entries["monitor.zex"]["type"], "ZEX")
        self.assertIn("sio.zmod", entries)
        production = json.loads(read_text(PROD_MANIFEST))
        pentries = {entry["name"]: entry for entry in production["files"]}
        self.assertEqual(pentries["monitor.zex"]["type"], "ZEX")
        self.assertIn("sio.zmod", pentries)

    def test_image_contains_the_monitor_command_surface(self):
        _p, image = self._read("build-m7-selftest/"
                               "zephyros-m7-selftest.bin")
        for marker in (b"ZephyrOS native monitor\r\n",
                       b"m7 selftest: OK\r\n",
                       b"system: abi ",
                       b"ERR: ARG\r\n",
                       b"ERR: COMMAND\r\n",
                       b"monitor: exit status 0x"):
            self.assertIn(marker, image,
                          "marker %r missing from the M7 image" % marker)

    def test_serial_smoke_m7_expectations_exist_verbatim(self):
        import serial_smoke
        _p, image = self._read("build-m7-selftest/"
                               "zephyros-m7-selftest.bin")
        for const in (serial_smoke.M7_SELFTEST_OK,
                      serial_smoke.M7_BANNER,
                      serial_smoke.M7_ERR_ARG):
            self.assertIn(const, image,
                          "serial_smoke expectation %r is not in the "
                          "image" % const)
        # The system-info and launcher lines are composed from linked
        # literal fragments (verified end to end by tests/test_m7_sim.py
        # against the serial_smoke constants); every fragment must be
        # present in the image.
        for fragment in (b"system: abi ", b" app ", b" os ", b" common ",
                         b" kernel-bank ", b" romfs ",
                         b"bank: current ", b" mask ",
                         b"monitor: exit status 0x", b" active ",
                         b" free-banks "):
            self.assertIn(fragment, image,
                          "expected output fragment %r is not in the "
                          "image" % fragment)


class TestMonitorSourceDependencies(unittest.TestCase):
    """Static proof that the monitor is console- and device-agnostic."""

    FORBIDDEN_PATTERNS = (
        (re.compile(r"\bearly_console\w*"), "early-console dependency"),
        (re.compile(r"\bSIO\w*|\bsio0\w*"), "SIO dependency"),
        # M8 device classes: the monitor must run unchanged over any
        # console binding and therefore knows none of them.
        (re.compile(r"\bv9958\b", re.IGNORECASE), "V9958 dependency"),
        (re.compile(r"\bhid\b", re.IGNORECASE), "HID dependency"),
        (re.compile(r"\bioc\b", re.IGNORECASE), "IOC dependency"),
        (re.compile(r"\bBDOS\b|\bBIOS\b|\bWBOOT\b"), "CP/M dependency"),
        (re.compile(r"\b__asm\w*\b"), "inline assembly in monitor C"),
        (re.compile(r"\b(IN|OUT)\s+A?\s*,?\s*\("), "direct port I/O"),
        (re.compile(r"0x00?5\b|0005h"), "CP/M CALL 5 vector"),
        (re.compile(r"\b0x2[0-3]\b|\b2[0-3]h\b"), "SIO0 port constant"),
        (re.compile(r"\b0x4[0-3]\b|\b4[0-3]h\b"), "CTC port constant"),
    )

    def test_monitor_sources_have_no_forbidden_dependencies(self):
        for path in MONITOR_SOURCES:
            text = read_text(path)
            if path.endswith(".asm"):
                body = "\n".join(line.split(";")[0]
                                 for line in text.splitlines())
            else:
                body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
                body = re.sub(r"//[^\n]*", "", body)
            for pattern, label in self.FORBIDDEN_PATTERNS:
                self.assertIsNone(
                    pattern.search(body),
                    "%s contains a %s: %r"
                    % (os.path.relpath(path, ROOT), label,
                       pattern.pattern))

    def test_monitor_asm_has_no_port_instructions(self):
        text = read_text(
            os.path.join(ROOT, "user", "monitor", "monitor_entry.asm"))
        body = "\n".join(line.split(";")[0] for line in text.splitlines())
        self.assertNotRegex(body, r"\b(in|out)\s")

    def test_runtime_diag_wrapper_uses_only_the_syscall_gate(self):
        text = read_text(RUNTIME_ASM)
        body = re.sub(r";[^\n]*", "", text)
        self.assertNotRegex(body, r"\b(in|out)\s")
        self.assertNotIn("0x22", body)
        self.assertNotIn("0x23", body)


class TestM7MapcheckRules(unittest.TestCase):
    def test_synthetic_map_with_m7_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_diag_service_must_be_bank7(self):
        text = make_map(symbols={"_zos_diag_mem_read": 0xE100})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_diag_prefix_sweep(self):
        text = make_map(symbols={"_zos_diag_future_service": 0xE140})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_missing_diag_symbol_rejected(self):
        text = make_map(symbols={"_zos_diag_io_out": None})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "map has no M7 symbol"):
            mapcheck.check_map(text)

    def test_production_rejects_m7_acceptance_symbols(self):
        text = make_map(symbols={"_m7_selftest_result": 0xC600})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "production image"):
            mapcheck.check_map(text, production=True)


class _FakeLink:
    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(data)

    def flush(self):
        pass


class TestM7SmokeSessionScript(unittest.TestCase):
    """The serial_smoke M7 session state machine: ordered steps,
    command sequence, prompt synchronisation, and completion."""

    TRANSCRIPT = (
        b"ZephyrOS native monitor\r\n"
        b"bank=00 pid=0 scratch=0B41\r\n"
        b"Z80> "
        b"H\r\n"
        b"Q              quit\r\n"
        b"Z80> "
        b"R\r\n"
        b"system: abi 01 app 10 os 18 common E000 kernel-bank 07 "
        b"romfs 02\r\n"
        b"process: pid 0 bank 0 flags 0001\r\n"
        b"bank: current 00 mask 7F\r\n"
        b"Z80> "
        b"M 0B41 A5\r\nOK\r\nZ80> "
        b"D 0B41 1\r\n0B41: A5 \r\nZ80> "
        b"D 0100 20\r\n0110: \r\nZ80> "
        b"I 20\r\nIN=FF\r\nZ80> "
        b"O 00 18\r\nERR: ARG\r\nZ80> "
        b"DB 7 0100 1\r\nERR: ARG\r\nZ80> "
        b"D ZZZ\r\nERR: ARG\r\nZ80> "
        b"Q\r\n"
        b"monitor: exit status 0x0000 active 0 free-banks 7\r\n"
        b"\r\nZephyrOS native monitor\r\n"
        b"bank=00 pid=0 scratch=0B41\r\nZ80> "
        b"R\r\nbank: current 00 mask 7F\r\nZ80> "
        b"Q\r\n"
        b"monitor: exit status 0x0000 active 0 free-banks 7\r\n"
    )

    def test_session_completes_and_sends_the_scripted_commands(self):
        import serial_smoke
        link = _FakeLink()
        session = serial_smoke._M7Session(link)
        session.send_delay = 0  # no pacing in the unit test
        done = False
        # Feed in chunks so the incremental update path is exercised.
        for offset in range(0, len(self.TRANSCRIPT), 32):
            if session.update(self.TRANSCRIPT[:offset + 32]):
                done = True
                break
        self.assertTrue(done, "session did not finish")
        self.assertIsNone(session.error)
        self.assertEqual(session.step, len(session.steps))
        raw = b"".join(link.written)
        commands = [line for line in raw.split(b"\r") if line]
        self.assertEqual(commands, [
            b"H", b"R", b"M 0B41 A5", b"D 0B41 1", b"D 0100 20",
            b"I 20", b"O 00 18", b"DB 7 0100 1", b"D ZZZ", b"Q",
            b"R", b"Q",
        ])


class _ScriptedLink:
    """Minimal pyserial stand-in: replays a payload once, then EOF."""

    def __init__(self, payload):
        self.payload = payload
        self.pos = 0
        self.written = []

    def read(self, size):
        chunk = self.payload[self.pos:self.pos + size]
        self.pos += len(chunk)
        return chunk

    def write(self, data):
        self.written.append(data)

    def flush(self):
        pass

    def close(self):
        pass


class _CapturedOutput:
    """redirect_stdout sink that also supplies sys.stdout.buffer."""

    def __init__(self):
        self.text = io.StringIO()
        self.buffer = io.BytesIO()

    def write(self, data):
        return self.text.write(data)

    def flush(self):
        pass


class TestM7SmokeBootGate(unittest.TestCase):
    """--mode m7 must be listening before the board boots: the boot-log
    gate, the no-commands-on-a-warm-board rule, and the operator hints."""

    WARM_TAIL = (
        b"ZephyrOS native monitor\r\n"
        b"bank=00 pid=0 scratch=0B41\r\n"
        b"Z80> "
    )

    def run_main(self, payload, timeout=0.05):
        import serial_smoke

        fake = types.ModuleType("serial")
        fake.EIGHTBITS = 8
        fake.PARITY_NONE = "N"
        fake.STOPBITS_ONE = 1
        fake.SerialException = type("SerialException", (Exception,), {})
        link = _ScriptedLink(payload)
        fake.Serial = lambda **kwargs: link
        previous = sys.modules.get("serial")
        sys.modules["serial"] = fake
        out, err = _CapturedOutput(), io.StringIO()
        try:
            with contextlib.redirect_stdout(out), \
                    contextlib.redirect_stderr(err):
                code = serial_smoke.main(
                    ["--mode", "m7", "--port", "FAKE",
                     "--timeout", str(timeout)])
        finally:
            if previous is None:
                del sys.modules["serial"]
            else:
                sys.modules["serial"] = previous
        return code, out.text.getvalue(), err.getvalue(), link

    def test_warm_board_fails_without_starting_the_session(self):
        code, out, err, link = self.run_main(self.WARM_TAIL)
        self.assertEqual(code, 1)
        self.assertEqual(link.written, [],
                         "no command may be sent before the boot log")
        self.assertIn("already running the monitor", out,
                      "warm-board reset hint missing")
        self.assertIn("no boot log was captured", err)

    def test_boot_log_starts_the_session_from_the_selftest_marker(self):
        import serial_smoke

        boot = (
            serial_smoke.BANNER
            + serial_smoke.CONSOLE_DYNAMIC_OK
            + serial_smoke.M7_SELFTEST_OK
            + b"\r\n"
            + self.WARM_TAIL
        )
        code, _out, _err, link = self.run_main(boot)
        self.assertEqual(code, 1, "session cannot complete: link is a stub")
        raw = b"".join(link.written)
        self.assertTrue(raw.startswith(b"H\r"),
                        "first paced command must be H; got %r" % raw)


if __name__ == "__main__":
    unittest.main()
