#!/usr/bin/env python3
"""Host-side monitor tests: ABI/tool constant parity, EXE layout,
source/map dependency inspection, and artifact integrity.

Coverage:

* diagnostic syscall numbers and packed-block constants are value-identical
  across abi/kos/diag.h and the SDK assembly wrappers in
  user/sdk/src/diaggate.asm;
* the monitor manifest/rights: ordinary EXE v2, entry at 0100h, exact
  linked image size, stack reservation, and a low-window scratch block;
* the linked monitor map contains only monitor/runtime modules - no
  kernel-private symbols, no SIO/early-console/CP/M dependencies - and
  the packed monitor.exe is stored byte-identically in the subsystem
  acceptance and production ROMFS images;
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
import exepack  # noqa: E402
from maptools import evaluate, make_map, parse_c_header  # noqa: E402

DIAG_H = os.path.join(ROOT, "abi", "kos", "diag.h")
RUNTIME_ASM = os.path.join(ROOT, "user", "sdk", "src", "diaggate.asm")
CONSOLE_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "console.h")
MONITOR_JSON = os.path.join(ROOT, "build", "user", "monitor", "monitor.json")
MONITOR_MAP = os.path.join(ROOT, "build", "user", "monitor", "monitor.map")
SUBSYS_MANIFEST = os.path.join(ROOT, "tests", "subsys", "manifest.json")
PROD_MANIFEST = os.path.join(ROOT, "recovery", "manifest.json")

MONITOR_SOURCES = (
    os.path.join(ROOT, "user", "monitor", "monitor.c"),
    os.path.join(ROOT, "user", "monitor", "monitor.h"),
    os.path.join(ROOT, "user", "monitor", "monitor_exec.c"),
    os.path.join(ROOT, "user", "monitor", "monitor_mem.c"),
    os.path.join(ROOT, "user", "monitor", "monitor_io.c"),
    os.path.join(ROOT, "user", "monitor", "hexfmt.c"),
    os.path.join(ROOT, "user", "monitor", "hexdump.c"),
    os.path.join(ROOT, "user", "monitor", "status.c"),
)

# The complete module set of the linked monitor (proves nothing from the
# kernel was dragged into the EXE link).
MONITOR_MODULES = {
    "kos_crt0", "kos_runtime", "kos_gate", "kos_diaggate",
    "kos_ipcgate", "nsgate", "args", "console_io", "console_proto",
    "ipc", "monitor", "monitor_exec", "monitor_mem", "monitor_io",
    "hexfmt", "hexdump", "status", "exec",
}

# Private kernel symbols that must never appear in a user EXE.  The
# runtime's own `kos_scratch` is a user-side object (M6), so the
# sweep names the kernel families instead of the bare `_kos_` prefix.
KERNEL_PREFIXES = (
    "_kos_syscall", "_kos_diag", "_kos_bank", "_kos_user", "_kos_process",
    "_process_", "_kernel", "_irq_", "_console_driver", "_console_forward",
    "_console_api", "_console_in", "_console_out", "_device_",
    "_romfs_", "_module_", "_mod_", "_exe_validate", "_early_console",
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
        self.diag = parse_c_header(DIAG_H)
        self.asm = parse_asm_constants(read_text(RUNTIME_ASM))

    def _diag(self, name):
        return evaluate(self.diag[name], self.diag)

    def test_syscall_numbers_match_the_assembly_wrappers(self):
        expected = {
            "SYS_DIAG_MEM_READ": 0x28,
            "SYS_DIAG_MEM_WRITE": 0x29,
            "SYS_DIAG_IO_IN": 0x2A,
            "SYS_DIAG_IO_OUT": 0x2B,
            "SYS_DIAG_BANK_INFO": 0x2C,
            "SYS_DIAG_SYSTEM_INFO": 0x2D,
        }
        for name, value in expected.items():
            self.assertEqual(
                self._diag(name.replace("SYS_", "KOS_SYSCALL_")),
                value, "%s is mispinned" % name)
            self.assertEqual(self.asm.get(name), value,
                             "%s differs from the SDK assembly wrapper"
                             % name)

    def test_range_and_mask_constants(self):
        for name, value in (("KOS_DIAG_MEM_BANK_MAX", 6),
                            ("KOS_DIAG_MEM_READ_MIN", 0x0000),
                            ("KOS_DIAG_MEM_READ_MAX", 0xDFFF),
                            ("KOS_DIAG_MEM_WRITE_MIN", 0x0100),
                            ("KOS_DIAG_MEM_WRITE_MAX", 0xDFFF),
                            ("KOS_DIAG_IO_PORT_MIN", 0x10),
                            ("KOS_DIAG_IO_PORT_MAX", 0xFF),
                            ("KOS_DIAG_BANK_MASK", 0x7F),
                            ("KOS_DIAG_SYSTEM_INFO_SIZE", 8),
                            ("KOS_DIAG_SYSTEM_INFO_ABI", 1)):
            self.assertEqual(self._diag(name), value,
                             "%s is mispinned" % name)

    def test_packed_block_asserts_present(self):
        text = re.sub(r"\s+", " ", read_text(DIAG_H))
        for statement in (
                "sizeof(struct kos_diag_mem_read_args) == 4",
                "sizeof(struct kos_diag_mem_write_args) == 4",
                "sizeof(struct kos_diag_io_in_args) == 1",
                "sizeof(struct kos_diag_io_out_args) == 2",
                "sizeof(struct kos_diag_system_info) == "
                "KOS_DIAG_SYSTEM_INFO_SIZE",
                "__builtin_offsetof(struct kos_diag_system_info, "
                "common_base) == 4",
                "__builtin_offsetof(struct kos_diag_system_info, "
                "kernel_bank) == 6",
                "__builtin_offsetof(struct kos_diag_system_info, "
                "romfs_first_page) == 7"):
            self.assertIn(statement, text,
                          "packed-layout assertion missing: %s" % statement)

    def test_line_limit_is_80_characters(self):
        console = parse_c_header(CONSOLE_H)
        self.assertEqual(evaluate(console["KOS_LINE_MAX"], console), 80)
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

    def test_manifest_declares_ordinary_exe_with_low_scratch(self):
        if not os.path.exists(MONITOR_JSON):
            self.skipTest("the monitor manifest has not been built")
        manifest = exepack.load_manifest(MONITOR_JSON)
        self.assertEqual(manifest["entry"], "kos_entry")
        # The SDK's low-data area (package 42b) is 93 bytes: the scratch
        # and raw request blocks, the call block and the nsgate blocks.
        self.assertEqual(manifest["entry_offset"], 93)
        self.assertEqual(manifest["scratch"]["symbol"], "kos_scratch")
        self.assertEqual(manifest["scratch"]["size"], 16)

    def test_packed_monitor_matches_the_manifest_and_report(self):
        _p, data = self._read("build/modules/monitor.exe")
        _p, report_text = self._read(
            "build/user/monitor/monitor.report.json")
        report = json.loads(report_text)
        if not os.path.exists(MONITOR_JSON):
            self.skipTest("the monitor manifest has not been built")
        manifest = exepack.load_manifest(MONITOR_JSON)
        fields, geo = exepack.validate_image(data)
        self.assertEqual(fields["image_size"], manifest["image_size"])
        self.assertEqual(fields["bss_size"], manifest["bss_size"])
        self.assertEqual(fields["stack_size"], manifest["stack_size"])
        self.assertEqual(geo["entry"], 0x015D)
        self.assertEqual(report["sha256"],
                         hashlib.sha256(data).hexdigest())
        self.assertEqual(report["scratch"], manifest["scratch"])
        self.assertEqual(report["entry"]["symbol"], "kos_entry")

    def test_linked_map_contains_only_monitor_and_runtime_modules(self):
        _p, raw = self._read("build/user/monitor/monitor.map")
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
        # The SDK's shared scratch symbol stays inside the caller window.
        for name in ("kos_scratch",):
            address = symbols[name]
            self.assertGreaterEqual(address, 0x0100)
            self.assertLess(address, 0x2000)

    def test_romfs_images_store_the_same_monitor_bytes(self):
        _p, packed = self._read("build/modules/monitor.exe")
        rels = ["build-subsys-selftest/recovery.romfs"]
        if os.path.exists(os.path.join(ROOT, "build", "recovery.romfs")):
            rels.append("build/recovery.romfs")
        for rel in rels:
            _p, stream = self._read(rel)
            _h, entries = romfspack.parse_image(stream)
            names = {entry["name"]: entry for entry in entries}
            self.assertIn("monitor.exe", names,
                          "%s does not carry monitor.exe" % rel)
            self.assertEqual(names["monitor.exe"]["flags"],
                             romfspack.FLAG_EXE)
            self.assertEqual(names["monitor.exe"]["payload"], packed,
                             "%s monitor.exe payload differs from the "
                             "packed artifact" % rel)

    def test_manifests_name_monitor_as_a_exe(self):
        accepted = json.loads(read_text(SUBSYS_MANIFEST))
        entries = {entry["name"]: entry for entry in accepted["files"]}
        self.assertEqual(entries["monitor.exe"]["type"], "EXE")
        self.assertIn("sio.mod", entries)
        production = json.loads(read_text(PROD_MANIFEST))
        pentries = {entry["name"]: entry for entry in production["files"]}
        self.assertEqual(pentries["monitor.exe"]["type"], "EXE")
        self.assertIn("sio.mod", pentries)

    def test_image_contains_the_monitor_command_surface(self):
        _p, image = self._read("build-subsys-selftest/"
                               "konaos-subsys-selftest.bin")
        for marker in (b"KonaOS native monitor\r\n",
                       b"system: abi ",
                       b"ERR: ARG\r\n",
                       b"ERR: COMMAND\r\n",
                       b"X path [tail]  run a EXE from /system\r\n",
                       b"exec: exit status 0x",
                       b"monitor: exit status 0x"):
            self.assertIn(marker, image,
                          "marker %r missing from the subsystem image"
                          % marker)
        # The system-info and launcher lines are composed from linked
        # literal fragments (replayed end to end by tests/test_subsys_sim.py);
        # every fragment must be present in the image.
        for fragment in (b" app ", b" os ", b" common ", b" kernel-bank ",
                         b" romfs ", b"bank: current ", b" mask ",
                         b" active ", b" free-banks "):
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

    def test_monitor_has_no_assembly(self):
        # The SDK owns crt0 and the gate stubs; the monitor is C only.
        for name in ("monitor_entry.asm",):
            self.assertFalse(
                os.path.exists(os.path.join(ROOT, "user", "monitor", name)),
                "the monitor still carries entry assembly")

    def test_sdk_diag_gate_uses_only_the_syscall_gate(self):
        text = read_text(RUNTIME_ASM)
        body = re.sub(r";[^\n]*", "", text)
        self.assertNotRegex(body, r"\b(in|out)\s")
        self.assertIn("rst\t0x30", body)


class TestM7MapcheckRules(unittest.TestCase):
    def test_synthetic_map_with_m7_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_diag_driver_must_be_bank7(self):
        text = make_map(symbols={"_kos_diag_mem_read": 0xE100})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_diag_prefix_sweep(self):
        text = make_map(symbols={"_kos_diag_future_driver": 0xE140})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_missing_diag_symbol_rejected(self):
        text = make_map(symbols={"_kos_diag_io_out": None})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "map has no M7 symbol"):
            mapcheck.check_map(text)

    def test_production_rejects_m7_acceptance_symbols(self):
        text = make_map(symbols={"_m7_selftest_result": 0xC600})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "production image"):
            mapcheck.check_map(text, production=True)


class _ScriptedLink:
    """Minimal pyserial stand-in: replays one payload, then EOF."""

    def __init__(self, payload):
        self.payload = payload
        self.pos = 0
        self.written = []

    def read(self, size):
        chunk = self.payload[self.pos:self.pos + size]
        self.pos += len(chunk)
        return chunk

    def write(self, data):
        self.written.append(bytes(data))

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


class TestSubsystemSmokeMode(unittest.TestCase):
    """--mode subsystem: the boot-log gate, the interactive Q step at the
    monitor prompt, and the exit-line finish.  This is the current
    replacement for the retired M7 monitor smoke session: the monitor
    session itself is replayed end to end by tests/test_subsys_sim.py."""

    def transcript(self):
        import serial_smoke
        return (
            serial_smoke.BANNER
            + serial_smoke.CONSOLE_DRIVER_OK
            + serial_smoke.SUBSYS_SYSCALL_OK
            + serial_smoke.SUBSYS_IRQ_OK
            + serial_smoke.SUBSYS_ROMFS_OK
            + serial_smoke.SUBSYS_PROCESS_OK
            + b"KonaOS native monitor\r\n"
            + serial_smoke.SUBSYS_PROMPT
            + serial_smoke.SUBSYS_EXIT
            + b"0000 active 0 free-banks 6\r\n"
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
                    ["--mode", "subsystem", "--port", "FAKE",
                     "--timeout", str(timeout)])
        finally:
            if previous is None:
                del sys.modules["serial"]
            else:
                sys.modules["serial"] = previous
        return code, out.text.getvalue(), err.getvalue(), link

    def test_complete_transcript_sends_q_and_passes(self):
        code, out, err, link = self.run_main(self.transcript())
        self.assertEqual(code, 0, err)
        self.assertEqual(b"".join(link.written), b"Q",
                         "the tool must send exactly one Q at the prompt")
        self.assertIn("serial_smoke: OK", out)

    def test_incomplete_boot_log_fails_without_sending_q(self):
        import serial_smoke

        payload = (serial_smoke.BANNER
                   + serial_smoke.CONSOLE_DRIVER_OK
                   + serial_smoke.SUBSYS_SYSCALL_OK)
        code, _out, err, link = self.run_main(payload)
        self.assertEqual(code, 1)
        self.assertEqual(link.written, [],
                         "no command may be sent before the boot log is "
                         "complete")
        self.assertIn("was not observed", err)


class TestProductionSmokeMode(unittest.TestCase):
    """--mode normal (make smoke-hardware): the production boot contract.

    The production console binds to the V9958, so the serial link only ever
    carries the early banner.  The contract is therefore what serial can
    observe: the four banner lines in order, then a quiet window with no
    PANIC, no failed console bind and no '<module>: unavailable' line.  A
    bench running the console-serial variant routes the whole console to
    serial and therefore gets the stronger check (console line + monitor
    banner + prompt); with build-selected consoles there is no fallback."""

    def transcript(self, diagnostics=b""):
        import serial_smoke
        return (serial_smoke.BANNER
                + diagnostics
                + serial_smoke.CONSOLE_DRIVER_OK
                + serial_smoke.MONITOR_BANNER
                + serial_smoke.SUBSYS_PROMPT)

    def run_main(self, payload, timeout=0.05, quiet=0):
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
                    ["--mode", "normal", "--port", "FAKE",
                     "--timeout", str(timeout), "--quiet", str(quiet)])
        finally:
            if previous is None:
                del sys.modules["serial"]
            else:
                sys.modules["serial"] = previous
        return code, out.text.getvalue(), err.getvalue()

    def test_sio_console_transcript_passes_the_strong_check(self):
        code, out, err = self.run_main(self.transcript())
        self.assertEqual(code, 0, err)
        self.assertIn("serial_smoke: OK", out)

    def test_sio_console_transcript_prints_no_display_hint(self):
        code, _out, err = self.run_main(self.transcript())
        self.assertEqual(code, 0, err)
        self.assertNotIn("V9958", err)

    def test_banner_alone_passes_with_an_operator_step(self):
        """The V9958-bound production boot is a PASS on serial alone."""
        import serial_smoke

        code, out, err = self.run_main(serial_smoke.BANNER)
        self.assertEqual(code, 0, err)
        self.assertIn("banner-only PASS", out)
        self.assertIn("operator step", out)
        self.assertIn("V9958", out)
        self.assertIn("Z80>", out)

    def test_optional_module_diagnostics_fail(self):
        """'<module>: unavailable' is a boot failure, never tolerated."""
        code, _out, err = self.run_main(
            self.transcript(diagnostics=b"v9958: unavailable 0x05\r\n"))
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_panic_after_the_banner_fails(self):
        import serial_smoke

        code, _out, err = self.run_main(
            serial_smoke.BANNER + b"PANIC: kernel: boot aborted\r\n")
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_unavailable_module_after_the_banner_fails(self):
        import serial_smoke

        code, _out, err = self.run_main(
            serial_smoke.BANNER + b"iocfat: unavailable 0x07\r\n")
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_missing_banner_line_fails(self):
        import serial_smoke

        # Drop the last banner line: three of four is not a boot.
        partial = serial_smoke.BANNER.rsplit(b"\r\n", 2)[0]
        code, _out, err = self.run_main(partial)
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_out_of_order_banner_fails(self):
        import serial_smoke

        lines = serial_smoke.BANNER.split(b"\r\n")
        swapped = b"".join(line + b"\r\n" for line in
                           [lines[1], lines[0]] + lines[2:])
        self.assertNotEqual(swapped, serial_smoke.BANNER)
        code, _out, err = self.run_main(swapped)
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_unbound_console_fails(self):
        import serial_smoke

        code, _out, err = self.run_main(
            serial_smoke.BANNER + b"console: unbound\r\n")
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)

    def test_failed_console_bind_fails(self):
        import serial_smoke

        code, _out, err = self.run_main(
            serial_smoke.BANNER + b"console: driver failed\r\n")
        self.assertEqual(code, 1)
        self.assertIn("was not observed", err)


if __name__ == "__main__":
    unittest.main()
