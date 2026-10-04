#!/usr/bin/env python3
"""Host-side functional test: replay the target-side M3 self-test in a
minimal Z80 simulator.

Executes the real linked kernel code from the m3-selftest image exactly as
the boot path would:

  1. the boot C-runtime initialization (zero/copy/GSINIT), because M3
     driver ops tables are initialized data,
  2. irq_init(), device_registry_init(), driver_init_static(), the M4
     module loader initialization and the embedded SIO module catalog
     load, and finally console_bind_defaults(),
  3. the full `zos_run_m3_selftest` C driver: registry semantics,
     checkpoint/rollback, exhaustion, ABI rejection, console TX/RX, and
     the static IRQ probe claiming CTC0 through the M2 executive (one
     injected vector per EI window while the probe waits).

The SIO model captures data-port writes (console transcript evidence) and
feeds one queued RX byte for the RX test.  Physical hardware remains
authoritative for real UART and CTC timing.

Skipped when the m3-selftest image has not been built
(`make m3-selftest`).
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from z80sim import Z80, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STEP_LIMIT = 8000000

SIO_INIT_SEQUENCE = bytes(
    [0x18, 0x04, 0x44, 0x03, 0xC1, 0x05, 0xEA, 0x01, 0x00]
)

M3_TX_OK = b"m3 selftest: console TX OK\r\n"
M3_RX_READY = b"m3 selftest: ready for RX\r\n"
M3_RX_OK = b"m3 selftest: RX OK\r\n"
M3_OK = b"m3 selftest: OK\r\n"
M3_RX_BYTE = 0x52  # 'R'


def v9958_glyph_text(peer):
    """Decode the text rendered by the V9958 module from its command
    trace: every 6x8 HMMM glyph copy lands on its destination cell."""
    cells = {}
    for cmd in peer.commands:
        block = cmd.get("block")
        if (cmd.get("code") != 0xD0 or not block or
                (block[8] | (block[9] << 8)) != 6 or
                (block[10] | (block[11] << 8)) != 8):
            continue
        sx = block[0] | (block[1] << 8)
        sy = block[2]
        dx = block[4] | (block[5] << 8)
        dy = block[6] | (block[7] << 8)
        glyph = ((sy & 0x3F) >> 3) * 32 + sx // 6
        if 32 <= glyph < 127:
            cells[(dy, dx // 6)] = chr(glyph)
    lines = []
    for y in sorted({row for row, _ in cells}):
        last = max(col for row, col in cells if row == y)
        lines.append("".join(cells.get((y, col), " ")
                             for col in range(last + 1)))
    return "\n".join(lines)


def run_function(z, addr, limit=STEP_LIMIT):
    """Call the function at `addr` and run until it returns."""
    z.push16(RET)
    z.pc = addr
    for _ in range(limit):
        z.step()
        if z.pc == RET:
            return
    raise AssertionError("function at %04X did not return (PC=%04X)"
                         % (addr, z.pc))


def run_m3_boot(z, syms):
    """Mirror kernel_main's device-layer boot section (without the
    ~1 s connect delay): registry, the embedded SIO module through the
    dynamic loader, then the console binding."""
    run_function(z, syms["__zos_crt0_init"])
    run_function(z, syms["_irq_init"])
    run_function(z, syms["_device_registry_init"])
    run_function(z, syms["_module_loader_init"])
    run_function(z, syms["_module_catalog_load_all"])
    run_function(z, syms["_console_bind_defaults"])


def run_m3_selftest(z, syms):
    """Run the C self-test, injecting CTC0 while the IRQ probe waits."""
    probe_wait = syms["_m3_irq_probe_wait"]
    hits_addr = syms["_m3_irq_probe_hits"]
    armed = False

    z.push16(RET)
    z.pc = syms["_zos_run_m3_selftest"]
    for _ in range(STEP_LIMIT):
        op = z.rd(z.pc)
        if probe_wait <= z.pc < probe_wait + 0x80 and op == 0xFB:
            armed = True
        if armed:
            hits = z.bank7[hits_addr] | (z.bank7[hits_addr + 1] << 8)
            if hits >= 3:
                armed = False
            elif z.iff1 and not z.ei_delay and not z.int_pending:
                z.assert_int(0x00)
        z.step()
        if z.pc == RET:
            break
    else:
        raise AssertionError("m3 selftest did not return (PC=%04X)" % z.pc)

    return z.bank7[syms["_m3_selftest_result"]]


class TestM3SelftestSim(unittest.TestCase):
    """Full target-side self-test replay (requires the m3-selftest image)."""

    def setUp(self):
        self.bin_path = os.path.join(
            ROOT, "build-m3-selftest", "zephyros-m3-selftest.bin"
        )
        if not os.path.exists(self.bin_path):
            self.skipTest("m3-selftest image missing; run `make m3-selftest`")
        self.image = open(self.bin_path, "rb").read()
        map_path = os.path.join(ROOT, "build-m3-selftest", "zephyros.map")
        self.syms = read_symbols(map_path)
        self.page0, self.page1 = self.image[:0x10000], self.image[0x10000:]

    def make_cpu(self):
        z = Z80(self.page0, self.page1)
        z.latch = 0x18          # mode 11 over bank 0 (post-boot state)
        z.sp = 0xFFFE           # common boot stack; not the IRQ stacks
        return z

    def test_full_selftest_round_trip(self):
        z = self.make_cpu()
        run_m3_boot(z, self.syms)

        # The SIO device arrives through the module catalog load only;
        # the retired static driver table must not be linked anywhere.
        run_function(z, self.syms["_module_count"])
        self.assertEqual(z.a, 1, "boot module not resident")
        run_function(z, self.syms["_module_free_granules"])
        self.assertEqual(z.a, 15, "boot module granule accounting")

        # The RX test consumes exactly the byte the operator/script sends.
        z.sio_rx.append(M3_RX_BYTE)
        result = run_m3_selftest(z, self.syms)
        self.assertEqual(result, 0x00,
                         "m3 selftest failed with code 0x%02X" % result)

        tx = bytes(z.sio_tx)
        for expected in (M3_TX_OK, M3_RX_READY, M3_RX_OK, M3_OK):
            self.assertIn(expected, tx,
                          "console transcript missing %r" % expected)
        self.assertFalse(z.sio_rx, "RX byte was not consumed")

    def test_sio_driver_reprograms_documented_state(self):
        z = self.make_cpu()
        run_m3_boot(z, self.syms)
        self.assertTrue(
            bytes(z.sio_ctrl_writes).endswith(SIO_INIT_SEQUENCE),
            "driver init did not write the 115200 8N1 sequence: %s"
            % bytes(z.sio_ctrl_writes).hex(),
        )

    def test_default_binding_selects_registered_handle(self):
        z = self.make_cpu()
        run_m3_boot(z, self.syms)
        run_function(z, self.syms["_console_output_handle"])
        out_handle = z.a
        run_function(z, self.syms["_console_input_handle"])
        in_handle = z.a
        self.assertNotEqual(out_handle, 0, "console output unbound")
        self.assertNotEqual(in_handle, 0, "console input unbound")
        # Both directions must have selected the single registered
        # console-capable device, and the handle must be in the issued
        # range 1..16.
        self.assertEqual(in_handle, out_handle)
        self.assertTrue(1 <= out_handle <= 16)

    def test_console_tx_reaches_the_sio_port(self):
        z = self.make_cpu()
        run_m3_boot(z, self.syms)
        # Write a known buffer through the console service.  The string is
        # placed in bank-7 RAM after the image; console_write copies it
        # byte-by-byte to the SIO data port.
        message = b"host replay\r\n"
        length = len(message)
        dest = 0xC400
        for i, byte in enumerate(message):
            z.bank7[dest + i] = byte
        # int console_write(const char *buf, uint16_t len): pointer in HL,
        # length in DE; 16-bit result in DE (sdcccall(1)).
        z.hl = dest
        z.de = length
        run_function(z, self.syms["_console_write"])
        self.assertEqual(z.de, length, "console_write returned a short count")
        self.assertEqual(bytes(z.sio_tx), message)


class TestM3ProductionBoot(unittest.TestCase):
    """Replay kernel_main on the production image (no self-test objects).

    The ~1 s serial-connect delay is skipped by recognizing its exported
    entry symbol, so the replay runs the real boot sequence: bank
    validation, irq_init, early banner, registry loader/console boot, and
    the console-service acceptance line, ending in the halt loop.
    """

    def setUp(self):
        bin_path = os.path.join(ROOT, "build", "zephyros.bin")
        map_path = os.path.join(ROOT, "build", "zephyros.map")
        if not os.path.exists(bin_path):
            self.skipTest("normal image missing; run make")
        image = open(bin_path, "rb").read()
        self.page0, self.page1 = image[:0x10000], image[0x10000:0x20000]
        self.rom_pages = {n: image[n * 0x10000:(n + 1) * 0x10000]
                          for n in range(2, 8)}
        self.syms = read_symbols(map_path)

    def test_boot_reaches_console_service_line(self):
        z = Z80(self.page0, self.page1, rom_pages=self.rom_pages)
        z.latch = 0x18
        z.sp = 0xFFFE
        # The production boot stack loads the V9958 video core; the peer
        # absorbs its A0h-A4h bring-up (the module's CE wait is not a
        # bounded poll).
        z.vdp_peer = V9958Peer()
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]

        delay = self.syms["_kernel_serial_connect_delay"]
        for _ in range(100000):
            z.step()
            if z.pc == delay:
                break
        else:
            self.fail("kernel_main never called the connect delay")

        # Skip the delay body: pop the return address the call pushed.
        z.pc = z.pop16()

        for _ in range(STEP_LIMIT):
            z.step()
            if z.halted:
                break
        else:
            self.fail("kernel_main did not reach the halt loop (PC=%04X)"
                      % z.pc)

        tx = bytes(z.sio_tx)
        for line in (b"ZephyrOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                     b"kernel entry: OK\r\n", b"serial: OK\r\n",
                     b"HALT\r\n"):
            self.assertIn(line, tx, "boot transcript missing %r" % line)
        self.assertNotIn(b"PANIC", tx)
        # The console-service acceptance line follows the bound console.
        # The production stack is console-capable (the V9958 module
        # registers ZCAP_CHAR_OUT|ZCAP_CONSOLE_OUT), and this replay
        # supplies the V9958 peer, so the line is rendered by the V9958
        # terminal on the output side instead of reaching the SIO port.
        self.assertNotIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn("console: dynamic sio0.b OK",
                      v9958_glyph_text(z.vdp_peer))


if __name__ == "__main__":
    unittest.main()
