"""The serial driver's exclusive hold behind `/dev/serial/0`.

The production replay arms the console's serial rescue path (three ESCs
on the serial line, so input comes from it and the tee mirrors console
output to it), types the card program `tests/serial/serial.c` on the
serial line and asserts, with every SIO0/B port access logged and
attributed to the driver or the program:

- INFO on any handle: direct I/O, the data port 22h and the control port
  23h, 115,200 baud;
- one exclusive hold, a second one refused with -KOS_EBUSY;
- while held the driver touches no SIO0/B port for any caller - the
  program's own ordinary handle and the console's rescue scan and tee:
  GETC answers -KOS_EAGAIN, POLL and READ 0, PUTC and WRITE count their
  bytes in the drop counter and answer as if sent, and a byte waiting in
  the receiver stays there;
- the program's direct I/O reaches the ports: a receive timeout, a byte
  sent and one received, and serial_write;
- the hold's end rewrites WR4, WR3, WR5 (RTS asserted) and WR1 and drains
  the receiver, at serial_release and again at the exit of a program
  still holding the port; afterwards the console's serial line works.

The static half pins the manifest's release declaration and the INFO
numbers.
"""

import json
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from maptools import evaluate, parse_c_header  # noqa: E402
from test_hwtest_sim import HwtestReplay  # noqa: E402

SERIAL_EXE = os.path.join(ROOT, "build", "user", "serial.exe")
MANIFEST = os.path.join(ROOT, "drivers", "sio", "sio.json")
PROTOCOL = os.path.join(ROOT, "abi", "kos", "protocol.h")
MODULES = os.path.join(ROOT, "build", "modules")

DATA, CTRL = 0x22, 0x23
ENQ = 0x05
# The channel's setup after its reset: WR4, WR3, WR5 with RTS, WR1.
SETUP = [0x04, 0x44, 0x03, 0xC1, 0x05, 0xEA, 0x01, 0x00]
ACQUIRE, RELEASE = 0xFE, 0xFF

# The lines the program prints while it holds the port the first time:
# each reaches the driver as a WRITE through the tee and is dropped.
HELD_LINES = ("serial: hold 0", "serial: busy -4", "serial: none -1",
              "serial: got 41", "serial: ops -9 0 0 0 3",
              "serial: console 0 0 0", "serial: write 4")
# The other dropped bytes of that hold: the program's PUTC and 3-byte
# WRITE on its ordinary handle, and the console PUTC the tee mirrors.
HELD_EXTRA = 1 + 3 + 1
INFO_LINE = "serial: info 0 01 22 23 00 C2 01 00"
STEP_LIMIT = 60_000_000


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class TestSerialContract(unittest.TestCase):
    def test_the_endpoint_declares_the_release(self):
        endpoint = json.loads(read(MANIFEST))["endpoints"][0]
        self.assertEqual((endpoint["class"], endpoint["instance"]), (1, 0))
        self.assertIs(endpoint.get("release"), True)
        self.assertNotIn("events", endpoint)

    def test_the_info_numbers(self):
        constants = parse_c_header(PROTOCOL)
        value = lambda name: evaluate(constants[name], constants)  # noqa: E731
        self.assertEqual(value("KSIO_OP_INFO"), 5)
        self.assertEqual([value(name) for name in (
            "KSIO_INFO_FLAGS", "KSIO_INFO_DATA", "KSIO_INFO_CTRL",
            "KSIO_INFO_BAUD")], [0, 1, 2, 3])
        self.assertEqual(value("KSIO_CAP_DIRECT_IO"), 0x01)


@unittest.skipUnless(os.path.exists(SERIAL_EXE), "run make")
class TestSerialHoldReplay(HwtestReplay):
    """The production image, the serial line and the card program."""

    def driver(self):
        """The sio image's hot range, its dispatch and its state."""
        with open(os.path.join(MODULES, "drivers.plan.json"),
                  encoding="utf-8") as handle:
            plan = json.load(handle)
        entry = [item for item in plan["drivers"]
                 if item["name"] == "sio.mod"][0]
        with open(os.path.join(MODULES, "sio.report.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        hot = [segment for segment in report["segments"]
               if segment["role"] == "hot"][0]
        state = entry["io_base"] + report["symbols"]["_sio0b_state"]
        return (range(entry["hot_base"], entry["hot_base"] + hot["image_size"]),
                report["endpoints"][0]["dispatch"], state)

    def test_the_hold_its_answers_and_its_end(self):
        with open(SERIAL_EXE, "rb") as handle:
            executable = handle.read()
        z = self.start(card=True, extra_files={"SERIAL.EXE": executable})
        hot, dispatch, state = self.driver()

        # Every SIO0/B access, with who made it and whether the port was
        # held at the time.
        log = []
        flags = {"held": False, "bank": None}
        events = []
        io_in, io_out = z.io_in, z.io_out

        def mine(pc):
            return pc in hot and z.bank_of(pc) == flags["bank"]

        def logged_in(port):
            value = io_in(port)
            if port & 0xFF in (DATA, CTRL):
                log.append(("in", port & 0xFF, value, mine(z.pc),
                            flags["held"]))
            return value

        def logged_out(port, value):
            if port & 0xFF in (DATA, CTRL):
                log.append(("out", port & 0xFF, value & 0xFF, mine(z.pc),
                            flags["held"]))
            return io_out(port, value)

        z.io_in, z.io_out = logged_in, logged_out

        def hook(_step):
            flags["bank"] = z.bank_of(z.pc)
            frame = z.hl
            op = z.rd(frame)
            arg1 = z.rd16((frame + 4) & 0xFFFF)
            events.append((op, flags["held"], len(log)))
            if op == ACQUIRE:
                flags["held"] = True
            elif op == RELEASE and arg1 != 0:
                # The hold's end: what is waiting and what was dropped.
                memory = z.banks[6]
                events.append(("release", bytes(z.sio_rx),
                               memory[state] | (memory[state + 1] << 8),
                               len(log)))
                flags["held"] = False
            return None

        stage = {"answered": False, "lane": len(z.sio_tx),
                 "trace": len(z.glyphs)}

        def tick(_step):
            fresh = bytes(z.sio_tx)[stage["lane"]:]
            if not stage["answered"] and bytes((ENQ,)) in fresh:
                z.sio_rx.extend(b"Ayz")
                stage["answered"] = True
            self.pump(z)
            trace = "".join(z.glyphs[stage["trace"]:])
            released = fresh.find(b"serial: release 0")
            return ("serial: PASS" in trace and released >= 0
                    and fresh.rfind(b"/> ") > released)

        # The gesture hands input to the serial line and arms the tee;
        # the command is typed there.
        z.sio_rx.extend(b"\x1b\x1b\x1b/fs/serial.exe\r")
        if self.drive(z, STEP_LIMIT, {dispatch}, hook, tick=tick,
                      every=0x1FF) is None:
            self.pump(z)
            self.fail("the program did not finish: %r / %r" % (
                "".join(z.glyphs[stage["trace"]:])[-300:],
                bytes(z.sio_tx)[stage["lane"]:][-200:]))

        trace = self.trace(z, stage["trace"])
        for line in (INFO_LINE,) + HELD_LINES + (
                "serial: release 0", "serial: hold2 0", "serial: PASS"):
            self.assertIn(line, trace)
        lane = bytes(z.sio_tx)[stage["lane"]:]
        # Before the hold and after its end the tee carries the lines;
        # while the port is held none of them reaches it.
        self.assertIn(INFO_LINE.encode() + b"\r\n", lane)
        self.assertIn(b"serial: release 0\r\n", lane)
        for line in HELD_LINES + ("serial: hold2 0", "serial: PASS"):
            self.assertNotIn(line.encode(), lane)

        # While held the driver made no access at all, the program made
        # all of them, and the bytes sent are the program's own.
        held = [entry for entry in log if entry[4]]
        self.assertEqual([entry for entry in held if entry[3]], [])
        self.assertEqual(bytes(entry[2] for entry in held
                               if entry[0] == "out" and entry[1] == DATA),
                         bytes((ENQ,)) + b"held")
        self.assertTrue(any(entry[0] == "in" and entry[1] == DATA
                            and entry[2] == ord("A") for entry in held))
        # ... while the driver was called for the console's scan and tee
        # and the program's ordinary handle: GETC, PUTC and WRITE.
        held_ops = {event[0] for event in events
                    if len(event) == 3 and event[1]}
        self.assertTrue({0, 1, 2, 3, 4} <= held_ops, held_ops)

        # Each hold's end: the accesses of that RELEASE call, up to the
        # driver's next call.
        ends = []
        for index, event in enumerate(events):
            if event[0] == "release":
                bound = (events[index + 1][2] if index + 1 < len(events)
                         else len(log))
                ends.append((event[1], event[2], log[event[3]:bound]))
        self.assertEqual(len(ends), 2, "serial_release and the exit")
        waiting, dropped, _calls = ends[0]
        self.assertEqual(waiting, b"yz",
                         "the receiver was read while the port was held")
        self.assertEqual(dropped, sum(len(line) + 2 for line in HELD_LINES)
                         + HELD_EXTRA)
        for _waiting, _dropped, calls in ends:
            self.assertTrue(all(entry[3] for entry in calls), calls)
            self.assertEqual([entry[2] for entry in calls
                              if entry[0] == "out" and entry[1] == CTRL],
                             SETUP)
            self.assertEqual([entry for entry in calls
                              if entry[0] == "out" and entry[1] == DATA], [])
        # The first end drained the two bytes left in the receiver.
        self.assertEqual(bytes(entry[2] for entry in ends[0][2]
                               if entry[0] == "in" and entry[1] == DATA),
                         b"yz")
        self.assertEqual(bytes(z.sio_rx), b"")

        # The hold is over: the serial line owns input again, and a
        # command typed there runs with its output on the line.
        since = len(z.sio_tx)
        z.sio_rx.extend(b"pwd\r")

        def answered(_step):
            return b"/\r\n" in bytes(z.sio_tx)[since:]

        if self.drive(z, STEP_LIMIT, tick=answered, every=0x1FF) is None:
            self.fail("the serial line did not run pwd: %r"
                      % bytes(z.sio_tx)[since:])


if __name__ == "__main__":
    unittest.main()
