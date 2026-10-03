#!/usr/bin/env python3
"""ZephyrOS physical-hardware serial smoke capture.

Opens the SIO0/B serial link (115200 8N1, raw) on the given device,
captures output with a bounded timeout, and verifies the expected output:

    --mode normal    the exact M0 banner, then the halt loop
    --mode selftest  the banner plus "syscall selftest: OK"
    --mode irq       the banner plus "irq selftest: OK"
    --mode m3        the M3 image: banner, "console: dynamic sio0.b OK"
                     through the normal console service (SIO loaded from
                     the embedded module catalog), all embedded
                     self-tests, and the interactive console RX check
                     (the tool sends 'R' when the kernel announces
                     "ready for RX")
    --mode m4        the M4 dynamic-driver acceptance image: banner, the
                     dynamic console line, "m4 loader selftest: OK",
                     "m4 selftest: sio at 0x....", and the final
                     "m4 selftest: OK"
    --mode m4-failure  the M4 failure image: banner, "console: dynamic
                     driver failed" (init rolled back, no device) and
                     "m4 failure selftest: OK"
    --mode m5        the M5 ROMFS success image: banner, the dynamic
                     console line (SIO loaded from the ROMFS recovery
                     store), and "m5 romfs selftest: OK"
    --mode m5-corrupt  the M5 corrupt-metadata image: banner and the
                     precise early-console panic marker; no dynamic
                     console line (the module is never executed)
    --mode m6        the M6 success image: banner, the dynamic console
                     line, two zextest runs (bank 0 and bank 1), the
                     repeated-cycle/two-bank/raw-return/malformed
                     milestones, and "m6 selftest: OK"
    --mode m6-failure  the M6 failure image: banner, the rejected
                     truncated ZEX rolling back cleanly, the valid copy
                     still running, and "m6 failure selftest: OK"
    --mode m7        the M7 native-monitor image: the kernel-side
                     "m7 selftest: OK", then a scripted interactive
                     monitor session (help, system info, dumps of the
                     monitor bank and a selected bank, scratch-byte
                     modify/readback, harmless port input, rejected
                     latch-port output, invalid bank 7, bad syntax) and
                     two Q exit/relaunch cycles with identical launcher
                     count lines.  The boot log is printed once, before
                     any monitor command can run: this mode must be
                     listening before the board boots.  Start it, then
                     power-cycle the board or press RESET while it
                     waits; an already-running board fails with an
                     explicit hint instead of a partial transcript.
    --mode m8-ioc  the M8 IOC image: banner, the dynamic console line,
                     the host contract level line (08h), the controller
                     firmware level returned by PING (4Ah for the pinned
                     deployment), four repeated PING exchanges, the bulk
                     write 1/128/512-byte sections with matching DONE
                     records, the bulk read-back of all three sections,
                     the delete proven by a reopen answered exactly with
                     the frozen no-file status (`m8 ioc: cleanup OK
                     0x34`), one HID semantic-status sample, and
                     "m8 ioc selftest: OK".
                     If the deployed firmware answers the writable /SHARED
                     open with the unknown-command class, the transcript
                     carries "m8 ioc: bulk write PENDING 0xFE" and "m8
                     ioc selftest: PENDING 0xFE" instead: the tool then
                     reports PENDING explicitly and exits nonzero (3),
                     never printing its OK result.  Any other marker
                     value is a failure.  The boot log is captured
                     once, so start this tool first and reset the board.
    --mode m8-hid  the M8 HID image: banner, the dynamic console line,
                     the HID keyboard registration, the scripted
                     USB-keyboard pass, the typed-byte echo lines in
                     exact order (`m8 hid: rx XX` for a B 3 Enter
                     Backspace Ctrl-X Arrow-Up), the forced SIO input
                     fallback byte, and "m8 hid selftest: OK".  This
                     pass needs a USB keyboard: when "m8 hid: ready for
                     input" appears, type exactly the listed keys (the
                     operator types; the tool validates the transcript
                     and sends the fallback `R` over the serial link
                     when the image asks for it).  The boot log is
                     captured once, so start this tool first and reset
                     the board.

The device is given with --port or the ZOS_SERIAL_PORT environment
variable (the SERIAL_PORT make variable is passed through).  A missing or
unopenable device fails clearly; ordinary host-only `make test` never
invokes this tool.

On benches where the target's reset line also resets the USB bridge, the
TTY drops off the bus when the board is reset.  The tool reopens the same
device path and keeps capturing.  The M8 IOC smoke image additionally
waits for the host sync sequence (M8IOC_HOST_SYNC) before it prints
anything, so mode m8-ioc sends that sequence once its port is open and
repeats it until the first byte arrives.

Usage:
    serial_smoke.py --port /dev/ttyUSB0 [--mode normal|selftest|irq|m3|m4|m4-failure|m5|m5-corrupt|m6|m6-failure|m7|m8-ioc]
                   [--baud 115200] [--timeout 15]

Exit status: 0 success, 1 output mismatch/incomplete, 2 device unavailable.
"""

import argparse
import os
import re
import sys
import time

BANNER = (
    b"ZephyrOS 0.0\r\n"
    b"platform: Zephyr-80\r\n"
    b"kernel entry: OK\r\n"
    b"serial: OK\r\n"
    b"HALT\r\n"
)
SELFTEST_OK = b"syscall selftest: OK\r\n"
IRQ_SELFTEST_OK = b"irq selftest: OK\r\n"
CONSOLE_DYNAMIC_OK = b"console: dynamic sio0.b OK\r\n"
CONSOLE_DYNAMIC_FAIL = b"console: dynamic driver failed\r\n"
M3_RX_PROMPT = b"m3 selftest: ready for RX\r\n"
M3_SELFTEST_OK = b"m3 selftest: OK\r\n"
M3_RX_BYTE = b"R"
M4_SELFTEST_OK = b"m4 loader selftest: OK\r\n"
M4_SIO_PREFIX = b"m4 selftest: sio at "
M4_ACCEPT_OK = b"m4 selftest: OK\r\n"
M4_FAILURE_OK = b"m4 failure selftest: OK\r\n"
M5_SELFTEST_OK = b"m5 romfs selftest: OK\r\n"
M5_CORRUPT_PANIC = b"PANIC: romfs: corrupt recovery image\r\n"
M6_ZEXTEST_BANK0 = b"zextest: bank=0 "
M6_ZEXTEST_BANK1 = b"zextest: bank=1 "
M6_ZEXTEST_OK = b"zextest: OK\r\n"
M6_STATUS_PASS = b"m6 selftest: zextest status 0xA500\r\n"
M6_STATUS_PASS_BANK1 = b"m6 selftest: zextest status 0xA501\r\n"
M6_REPEAT_OK = b"m6 selftest: repeated cycles OK\r\n"
M6_TWO_BANK_OK = b"m6 selftest: two-bank OK\r\n"
M6_RAW_RETURN_OK = b"m6 selftest: raw return EFAULT\r\n"
M6_REJECTED_OK = b"m6 selftest: malformed rejected\r\n"
M6_SELFTEST_OK = b"m6 selftest: OK\r\n"
M6_FAILURE_OK = b"m6 failure selftest: OK\r\n"

# --- M7 native monitor ------------------------------------------------------
M7_SELFTEST_OK = b"m7 selftest: OK\r\n"
M7_PROMPT = b"Z80> "
M7_BANNER = b"ZephyrOS native monitor\r\n"
M7_SYSINFO = (b"system: abi 01 app 10 os 18 common E000 kernel-bank 07 "
              b"romfs 02\r\n")
M7_BANK_INFO = b"bank: current 00 mask 7F\r\n"
M7_OK = b"OK\r\n"
M7_ERR_ARG = b"ERR: ARG\r\n"
M7_EXIT = b"monitor: exit status 0x0000 active 0 free-banks 7\r\n"
M7_SCRATCH_RE = re.compile(rb"scratch=([0-9A-F]{4})")

# --- M8 IOC command + bulk transport ----------------------------------------
M8IOC_TRANSPORT_LEVEL = b"m8 ioc: transport level 08\r\n"
M8IOC_CONTROLLER_LEVEL = b"m8 ioc: controller level 4A\r\n"
M8IOC_PING_OK = b"m8 ioc: ping x4 OK\r\n"
M8IOC_BULK_WRITE_OK = b"m8 ioc: bulk write 1/128/512 OK\r\n"
# The PENDING outcome is exactly the verified unknown-command class 0FEh;
# any other code in either marker is a failure, never PENDING.
M8IOC_BULK_WRITE_PENDING_RE = re.compile(
    rb"m8 ioc: bulk write PENDING 0xFE\r\n")
M8IOC_BULK_READ_OK = b"m8 ioc: bulk read 1/128/512 OK\r\n"
M8IOC_DONE_OK = b"m8 ioc: done x3 OK\r\n"
# Deletion is proven only by the frozen no-file status 34h; any other
# code (a media/filesystem error, for example) is not cleanup evidence.
M8IOC_CLEANUP_RE = re.compile(rb"m8 ioc: cleanup OK 0x34\r\n")
M8IOC_HID_STATUS_RE = re.compile(rb"m8 ioc: hid status [0-9A-F]{2}\r\n")
M8IOC_SELFTEST_OK = b"m8 ioc selftest: OK\r\n"
M8IOC_SELFTEST_PENDING_RE = re.compile(
    rb"m8 ioc selftest: PENDING 0xFE\r\n")
M8IOC_FAIL = b"m8 ioc selftest: FAIL"
M8IOC_UNAVAILABLE = b"ioc: unavailable\r\n"

# --- M8 HID keyboard/text input ---------------------------------------------
M8HID_KEYBOARD_OK = b"m8 hid: keyboard0 OK\r\n"
M8HID_READY = b"m8 hid: ready for input\r\n"
# The MCU's translation of the scripted key pass: a, B, 3, Enter,
# Backspace, Ctrl-X, Arrow-Up (ESC [ A).  Every byte must appear exactly
# once, in this order, in the `m8 hid: rx XX` echo lines.
M8HID_RX_RE = re.compile(rb"m8 hid: rx ([0-9A-F]{2})\r\n")
M8HID_TYPED_BYTES = (0x61, 0x42, 0x33, 0x0D, 0x08, 0x18, 0x1B, 0x5B, 0x41)
M8HID_TYPED_OK = b"m8 hid: typed pass OK\r\n"
M8HID_SIO_PROMPT = b"m8 hid: sio fallback, send R\r\n"
M8HID_FALLBACK_OK = b"m8 hid: sio fallback OK\r\n"
M8HID_SELFTEST_OK = b"m8 hid selftest: OK\r\n"
M8HID_FAIL = b"m8 hid selftest: FAIL"
M8HID_UNAVAILABLE = b"hid: unavailable\r\n"
M8HID_FALLBACK_BYTE = b"R"

# Host sync the smoke image waits for before the banner (bench quirk: the
# board's reset line also resets the USB bridge, so the board must not
# print until the host has reopened its port).  Mirrors
# ZOS_SMOKE_HOST_SYNC_FIRST/SECOND in platform/zephyr80/hardware.{h,inc};
# modes m8-ioc and m8-hid send it on every open and repeat it until the
# first byte.
M8IOC_HOST_SYNC = b"\xaa\x55"


def _m8ioc_complete(data):
    """True when the full write/read/cleanup/OK transcript is present."""
    if not (BANNER in data and CONSOLE_DYNAMIC_OK in data and
            M8IOC_TRANSPORT_LEVEL in data and
            M8IOC_CONTROLLER_LEVEL in data and M8IOC_PING_OK in data and
            M8IOC_SELFTEST_OK in data):
        return False
    return (M8IOC_BULK_WRITE_OK in data and M8IOC_DONE_OK in data and
            M8IOC_BULK_READ_OK in data and
            M8IOC_CLEANUP_RE.search(data) is not None)


def _m8ioc_pending(data):
    """True only for the documented PENDING outcome: the deployed firmware
    answered the writable /SHARED open with the unknown-command class.
    It never mixes with the OK path and is never acceptance evidence."""
    if (M8IOC_SELFTEST_OK in data or M8IOC_BULK_WRITE_OK in data or
            M8IOC_DONE_OK in data or M8IOC_BULK_READ_OK in data or
            M8IOC_CLEANUP_RE.search(data) is not None):
        return False
    return (BANNER in data and CONSOLE_DYNAMIC_OK in data and
            M8IOC_TRANSPORT_LEVEL in data and
            M8IOC_CONTROLLER_LEVEL in data and M8IOC_PING_OK in data and
            M8IOC_BULK_WRITE_PENDING_RE.search(data) is not None and
            M8IOC_SELFTEST_PENDING_RE.search(data) is not None)


def _m8ioc_result(data):
    """The m8-ioc outcome: "ok", "pending" or "fail", mutually
    exclusive.  PENDING is its own nonzero result - never a pass."""
    if M8IOC_FAIL in data or M8IOC_UNAVAILABLE in data or b"PANIC" in data:
        return "fail"
    if _m8ioc_complete(data):
        if (M8IOC_HID_STATUS_RE.search(data) is not None and
                b"FAIL" not in data):
            return "ok"
        return "fail"
    if _m8ioc_pending(data):
        return "pending"
    return "fail"


def _m8hid_result(data):
    """The m8-hid outcome: "ok" only when the boot log, the HID
    registration, the scripted typed-byte echo lines (exact bytes, exact
    order), the forced SIO fallback byte and the final marker all
    completed.  The USB-keyboard half cannot be verified without the
    echoed transcript, so a missing byte is a failure."""
    if M8HID_FAIL in data or M8HID_UNAVAILABLE in data or \
            b"PANIC" in data:
        return "fail"
    for needle in (BANNER, CONSOLE_DYNAMIC_OK, M8HID_KEYBOARD_OK,
                   M8HID_READY, M8HID_TYPED_OK, M8HID_SIO_PROMPT,
                   M8HID_FALLBACK_OK, M8HID_SELFTEST_OK):
        if needle not in data:
            return "fail"
    found = tuple(int(match.group(1), 16)
                  for match in M8HID_RX_RE.finditer(data))
    if found != M8HID_TYPED_BYTES:
        return "fail"
    return "ok"

# Per-byte pacing for scripted monitor commands.  The console RX path is
# a polled SIO register (no burst buffering), so a command sent as one
# fast burst can overrun while the monitor is echoing/printing; sending
# at typed-input speed keeps the script reliable on hardware.
M7_SEND_DELAY = 0.002


def _expect_bytes(needle):
    def check(tail):
        index = tail.find(needle)
        return None if index < 0 else index + len(needle)
    return check


def _expect_regex(pattern):
    compiled = re.compile(pattern)

    def check(tail):
        match = compiled.search(tail)
        return None if match is None else match.end()
    return check


class _M7Session:
    """Scripted interactive acceptance for the M7 monitor image.

    Steps run strictly in order; each command is sent only after the
    previous response has fully completed (its trailing prompt was
    observed), and command bytes are paced like typed input.  Both rules
    are required on hardware: the polled console RX register has no
    burst buffering, so input sent during output, or faster than the
    monitor's echo loop, is lost.  Each step then waits for its marker
    in the newly received bytes after the previous step, so repeated
    text (the prompt, OK, ERR: ARG) cannot satisfy a later step early.
    The operator scratch address is taken from the monitor banner and
    used for the modify/readback step.
    """

    send_delay = M7_SEND_DELAY

    def __init__(self, link):
        self.link = link
        self.pos = 0
        self.step = 0
        self.scratch = None
        self.error = None
        h = _expect_bytes
        r = _expect_regex

        def send_m():
            return b"M %04X A5" % self.scratch

        def send_d():
            return b"D %04X 1" % self.scratch

        def readback(tail):
            needle = ("%04X: A5 " % self.scratch).encode("ascii")
            index = tail.find(needle)
            return None if index < 0 else index + len(needle)

        self.steps = [
            (None, h(M7_PROMPT), None),
            (b"H", h(b"Q              quit\r\n"), None),
            (None, h(M7_PROMPT), None),
            (b"R", h(M7_SYSINFO), self._grab_scratch),
            (None, h(M7_PROMPT), None),
            (send_m, h(M7_OK), None),
            (None, h(M7_PROMPT), None),
            (send_d, readback, None),
            (None, h(M7_PROMPT), None),
            (b"D 0100 20", h(b"0110: "), None),
            (None, h(M7_PROMPT), None),
            (b"I 20", r(rb"IN=[0-9A-F]{2}\r\n"), None),
            (None, h(M7_PROMPT), None),
            (b"O 00 18", h(M7_ERR_ARG), None),
            (None, h(M7_PROMPT), None),
            (b"DB 7 0100 1", h(M7_ERR_ARG), None),
            (None, h(M7_PROMPT), None),
            (b"D ZZZ", h(M7_ERR_ARG), None),
            (None, h(M7_PROMPT), None),
            (b"Q", h(M7_EXIT), None),
            (None, h(M7_PROMPT), None),
            (b"R", h(M7_BANK_INFO), None),
            (None, h(M7_PROMPT), None),
            (b"Q", h(M7_EXIT), None),
        ]
        self.sent = False

    def _grab_scratch(self, data):
        match = M7_SCRATCH_RE.search(data)
        if match is None:
            self.error = "monitor banner did not print scratch=XXXX"
            return
        self.scratch = int(match.group(1), 16)

    def _send(self, line):
        """Send one command line paced byte-by-byte (typed-input speed)."""
        data = line + b"\r"
        for index, byte in enumerate(data):
            self.link.write(bytes((byte,)))
            self.link.flush()
            if self.send_delay and index + 1 < len(data):
                time.sleep(self.send_delay)

    def update(self, data):
        """Feed accumulated data; returns True when the script is done."""
        while self.step < len(self.steps):
            send, check, after = self.steps[self.step]
            if send is not None and not self.sent:
                line = send() if callable(send) else send
                self._send(line)
                self.sent = True
            tail = data[self.pos:]
            end = check(tail)
            if end is None:
                return False
            self.pos += end
            self.step += 1
            self.sent = False
            if after is not None:
                after(data)
                if self.error is not None:
                    return True
        return True


def main(argv):
    parser = argparse.ArgumentParser(description="ZephyrOS serial smoke check")
    parser.add_argument("--port", default=os.environ.get("ZOS_SERIAL_PORT", ""))
    parser.add_argument(
        "--baud", type=int, default=int(os.environ.get("ZOS_SERIAL_BAUD", "115200"))
    )
    parser.add_argument("--mode", choices=("normal", "selftest", "irq", "m3",
                                           "m4", "m4-failure", "m5",
                                           "m5-corrupt", "m6", "m6-failure",
                                           "m7", "m8-ioc", "m8-hid"),
                        default="normal")
    parser.add_argument(
        "--timeout", type=float, default=float(os.environ.get("ZOS_SERIAL_TIMEOUT", "15"))
    )
    args = parser.parse_args(argv)

    if not args.port:
        print(
            "serial_smoke: no serial device given; use --port or ZOS_SERIAL_PORT",
            file=sys.stderr,
        )
        return 2

    try:
        import serial
    except ImportError:
        print("serial_smoke: pyserial is not installed (pip install pyserial)",
              file=sys.stderr)
        return 2

    def _open_link():
        return serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.2,
        )

    try:
        link = _open_link()
    except serial.SerialException as exc:
        print("serial_smoke: cannot open %s: %s" % (args.port, exc), file=sys.stderr)
        return 2

    print("serial_smoke: listening on %s at %d 8N1 (mode=%s, timeout=%gs)"
          % (args.port, args.baud, args.mode, args.timeout))
    if args.mode == "m7":
        print("serial_smoke: mode m7 checks the boot log; power-cycle the "
              "board or press RESET now so the boot is captured")
    if args.mode == "m8-ioc":
        print("serial_smoke: mode m8-ioc checks the boot log; "
              "power-cycle the board or press RESET now so the boot "
              "(including the IOC selftest) is captured; the image waits "
              "for the host sync sequence, which this tool sends whenever "
              "the port is open")
    if args.mode == "m8-hid":
        print("serial_smoke: mode m8-hid checks the boot log and the "
              "scripted console phases; power-cycle the board or press "
              "RESET now so the boot (including the HID selftest) is "
              "captured; the image waits for the host sync sequence")
        print("serial_smoke: when 'm8 hid: ready for input' appears, type "
              "on the USB keyboard exactly: a B 3 Enter Backspace Ctrl-X "
              "Arrow-Up; the tool validates the echo and sends the "
              "fallback R over the serial link when asked")
    deadline = time.monotonic() + args.timeout
    data = b""
    bridge_drops = 0
    host_sync_at = 0.0
    rx_byte_sent = False
    m7_session = None
    m7_session_base = 0
    m7_warm_hinted = False
    try:
        while time.monotonic() < deadline:
            if link is None:
                # The board's reset line can reset the USB bridge itself
                # (observed on this bench): the TTY drops off the bus while
                # the bridge's UART keeps receiving into its buffer.  Reopen
                # the same device path and keep capturing; bytes that
                # arrived while USB re-enumerated are delivered on reopen.
                try:
                    link = _open_link()
                except (serial.SerialException, OSError):
                    time.sleep(0.2)
                    continue
                print("serial_smoke: %s is back (drop %d); still capturing"
                      % (args.port, bridge_drops))
                host_sync_at = 0.0
            if (args.mode in ("m8-ioc", "m8-hid") and
                    BANNER not in data and
                    time.monotonic() >= host_sync_at):
                # The smoke image holds the banner for this sequence; keep
                # sending until the boot log actually arrives.  The board
                # discards bytes that do not complete the sequence, and a
                # board already past its gate may echo stray bytes (e.g.
                # the monitor echoing input), so "any byte received" must
                # not stop the nudging.
                host_sync_at = time.monotonic() + 1.0
                try:
                    link.write(M8IOC_HOST_SYNC)
                    link.flush()
                except (serial.SerialException, OSError):
                    pass  # the read below notices a dropped link
            try:
                chunk = link.read(256)
            except (serial.SerialException, OSError) as exc:
                bridge_drops += 1
                print("serial_smoke: %s dropped (%s); waiting for it to "
                      "re-enumerate" % (args.port, exc))
                try:
                    link.close()
                except Exception:
                    pass
                link = None
                continue
            if chunk:
                data += chunk
                if args.mode == "m7":
                    if m7_session is None:
                        if (BANNER in data and CONSOLE_DYNAMIC_OK in data
                                and M7_SELFTEST_OK in data):
                            # Boot log captured; run the session strictly
                            # from the bytes after the selftest marker so
                            # a stale warm prompt (if any) cannot satisfy
                            # the first step early.
                            m7_session_base = (data.find(M7_SELFTEST_OK)
                                               + len(M7_SELFTEST_OK))
                            m7_session = _M7Session(link)
                        elif M7_PROMPT in data and not m7_warm_hinted:
                            m7_warm_hinted = True
                            print("serial_smoke: the board is already "
                                  "running the monitor; press RESET (or "
                                  "power-cycle) now to capture the boot "
                                  "log")
                    if m7_session is not None and \
                            m7_session.update(data[m7_session_base:]):
                        break
                elif args.mode == "normal" and BANNER in data:
                    break
                elif args.mode == "selftest" and BANNER in data and SELFTEST_OK in data:
                    break
                elif args.mode == "irq" and BANNER in data and IRQ_SELFTEST_OK in data:
                    break
                elif args.mode == "m3":
                    if M3_RX_PROMPT in data and not rx_byte_sent:
                        link.write(M3_RX_BYTE)
                        link.flush()
                        rx_byte_sent = True
                        print("serial_smoke: sent RX byte %r" % M3_RX_BYTE)
                    if (CONSOLE_DYNAMIC_OK in data and
                            M3_SELFTEST_OK in data):
                        break
                elif args.mode == "m4":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M4_SELFTEST_OK in data and
                            M4_SIO_PREFIX in data and
                            M4_ACCEPT_OK in data):
                        break
                elif args.mode == "m4-failure":
                    if (BANNER in data and CONSOLE_DYNAMIC_FAIL in data and
                            M4_FAILURE_OK in data):
                        break
                elif args.mode == "m5":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M5_SELFTEST_OK in data):
                        break
                elif args.mode == "m5-corrupt":
                    if BANNER in data and M5_CORRUPT_PANIC in data:
                        break
                elif args.mode == "m6":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M6_STATUS_PASS in data and
                            M6_STATUS_PASS_BANK1 in data and
                            M6_TWO_BANK_OK in data and
                            M6_REPEAT_OK in data and
                            M6_RAW_RETURN_OK in data and
                            M6_REJECTED_OK in data and
                            M6_SELFTEST_OK in data):
                        break
                elif args.mode == "m6-failure":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M6_ZEXTEST_OK in data and
                            M6_FAILURE_OK in data):
                        break
                elif args.mode == "m8-ioc":
                    if (_m8ioc_complete(data) or _m8ioc_pending(data)):
                        break
                elif args.mode == "m8-hid":
                    if M8HID_SIO_PROMPT in data and not rx_byte_sent:
                        try:
                            link.write(M8HID_FALLBACK_BYTE)
                            link.flush()
                        except (serial.SerialException, OSError):
                            pass  # the read notices a dropped link
                        rx_byte_sent = True
                        print("serial_smoke: sent fallback byte %r"
                              % M8HID_FALLBACK_BYTE)
                    if _m8hid_result(data) == "ok" or \
                            M8HID_FAIL in data or \
                            M8HID_UNAVAILABLE in data:
                        break
    finally:
        if link is not None:
            try:
                link.close()
            except Exception:
                pass

    print("serial_smoke: captured %d bytes (%d USB bridge drop(s) during "
          "capture)" % (len(data), bridge_drops))
    print("serial_smoke: transcript follows")
    sys.stdout.buffer.write(data + (b"" if data.endswith(b"\n") else b"\n"))

    if args.mode == "normal":
        ok = BANNER in data
        expected = "the exact M0 banner"
    elif args.mode == "selftest":
        ok = BANNER in data and SELFTEST_OK in data
        expected = "the banner plus 'syscall selftest: OK'"
    elif args.mode == "irq":
        ok = BANNER in data and IRQ_SELFTEST_OK in data
        expected = "the banner plus 'irq selftest: OK'"
    elif args.mode == "m4":
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              M4_SELFTEST_OK in data and M4_SIO_PREFIX in data and
              M4_ACCEPT_OK in data)
        expected = ("the banner, the dynamic console line, the loader "
                    "self-test, the dynamic SIO acceptance line, and "
                    "'m4 selftest: OK'")
    elif args.mode == "m4-failure":
        ok = (BANNER in data and CONSOLE_DYNAMIC_FAIL in data and
              M4_FAILURE_OK in data and CONSOLE_DYNAMIC_OK not in data)
        expected = ("the banner, 'console: dynamic driver failed' (no "
                    "dynamic console line), and 'm4 failure selftest: OK'")
    elif args.mode == "m5":
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              M5_SELFTEST_OK in data and b"FAIL" not in data and
              b"PANIC" not in data)
        expected = ("the banner, the dynamic console line (SIO loaded "
                    "from ROMFS), and 'm5 romfs selftest: OK'")
    elif args.mode == "m5-corrupt":
        ok = (BANNER in data and M5_CORRUPT_PANIC in data and
              CONSOLE_DYNAMIC_OK not in data and
              CONSOLE_DYNAMIC_FAIL not in data and
              M5_SELFTEST_OK not in data)
        expected = ("the banner and the early-console marker "
                    "'PANIC: romfs: corrupt recovery image' with no module "
                    "execution")
    elif args.mode == "m6":
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              M6_ZEXTEST_BANK0 in data and M6_ZEXTEST_BANK1 in data and
              M6_ZEXTEST_OK in data and M6_STATUS_PASS in data and
              M6_STATUS_PASS_BANK1 in data and M6_REPEAT_OK in data and
              M6_TWO_BANK_OK in data and M6_RAW_RETURN_OK in data and
              M6_REJECTED_OK in data and M6_SELFTEST_OK in data and
              b"FAIL" not in data and b"PANIC" not in data)
        expected = ("the banner, the dynamic console line, zextest in "
                    "bank 0 and bank 1, the repeated-cycle/two-bank/"
                    "raw-return/malformed milestones, and "
                    "'m6 selftest: OK'")
    elif args.mode == "m6-failure":
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              M6_ZEXTEST_OK in data and M6_FAILURE_OK in data and
              M6_SELFTEST_OK not in data and b"FAIL" not in data and
              b"PANIC" not in data)
        expected = ("the banner, the dynamic console line, the valid "
                    "copy running after the truncated ZEX rolled back, "
                    "and 'm6 failure selftest: OK'")
    elif args.mode == "m7":
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              M7_SELFTEST_OK in data and M7_BANNER in data and
              m7_session is not None and m7_session.step == len(m7_session.steps)
              and m7_session.error is None and
              data.count(M7_EXIT) == 2 and
              b"m7 selftest: FAIL" not in data and
              b"FAIL" not in data and b"PANIC" not in data)
        expected = ("the kernel-side 'm7 selftest: OK', the monitor "
                    "banner and prompt, the scripted help/system-info/"
                    "dump/modify/readback/port/rejection commands, and "
                    "two identical Q exit/relaunch count lines")
    elif args.mode == "m8-ioc":
        verdict = _m8ioc_result(data)
        ok = verdict == "ok"
        if verdict == "pending":
            print("serial_smoke: PENDING - the deployed firmware answers "
                  "the writable /SHARED open with the unknown-command "
                  "class; the bulk-write criterion is not acceptance "
                  "evidence")
            return 3
        expected = ("the banner, the dynamic console line, host transport "
                    "level 08h, the controller level 4Ah returned by PING, "
                    "repeated PINGs, the bulk 1/128/512 write with matching "
                    "DONE records, the read-back of all three sections, "
                    "the delete proven by a reopen answered with the "
                    "frozen no-file status, a HID semantic-status sample, "
                    "and 'm8 ioc selftest: OK'")
    elif args.mode == "m8-hid":
        ok = _m8hid_result(data) == "ok"
        expected = ("the banner, the dynamic console line, the HID "
                    "keyboard registration, the scripted USB-keyboard "
                    "typed-byte echo lines in exact order (a B 3 Enter "
                    "Backspace Ctrl-X Arrow-Up), the forced SIO fallback "
                    "byte, and 'm8 hid selftest: OK'")
    else:
        ok = (BANNER in data and CONSOLE_DYNAMIC_OK in data and
              SELFTEST_OK in data and IRQ_SELFTEST_OK in data and
              M3_SELFTEST_OK in data)
        expected = ("the banner, the dynamic console line, syscall/irq/m3 "
                    "self-tests, and the console RX round trip")

    if not ok:
        _explain_wrong_image(args.mode, data)
        print("serial_smoke: FAIL - %s was not observed (timeout %gs)"
              % (expected, args.timeout), file=sys.stderr)
        return 1
    print("serial_smoke: OK (%s)" % expected)
    if bridge_drops:
        print("serial_smoke: note: the USB bridge dropped %d time(s) during "
              "this capture; the transcript above was reassembled across "
              "the reconnect(s)" % bridge_drops)
    return 0


def _explain_wrong_image(mode, data):
    """Turn an image/mode mismatch into an actionable hint.

    The M5 success and corrupt images are flashed separately; a run with
    the wrong --mode otherwise fails with an unexplained timeout on the
    wrong transcript.
    """
    if mode == "m5":
        if M5_CORRUPT_PANIC in data:
            if b"header 5A 52 46 53" in data:
                print("serial_smoke: hint: the corrupt-recovery-image marker "
                      "was observed; the m5-corrupt image is probably flashed "
                      "(use --mode m5-corrupt)", file=sys.stderr)
            else:
                print("serial_smoke: hint: ROMFS init failed and the staged "
                      "header does not show 'ZRFS'; the diagnostic line above "
                      "carries the raw code, live latch, and staged bytes - "
                      "keep it for the record", file=sys.stderr)
        elif CONSOLE_DYNAMIC_OK in data and M5_SELFTEST_OK not in data:
            print("serial_smoke: hint: the dynamic console bound but no M5 "
                  "selftest marker appeared; the production image "
                  "(build/zephyros.bin) may be flashed instead of "
                  "build-m5-selftest/zephyros-m5-selftest.bin",
                  file=sys.stderr)
    elif mode == "m5-corrupt":
        if M5_SELFTEST_OK in data:
            print("serial_smoke: hint: the ROMFS success marker was observed; "
                  "the M5 success image is probably flashed "
                  "(use --mode m5)", file=sys.stderr)
        elif CONSOLE_DYNAMIC_OK in data and M5_CORRUPT_PANIC not in data:
            print("serial_smoke: hint: the dynamic console bound instead of "
                  "the panic marker; the production or an older image may be "
                  "flashed instead of "
                  "build-m5-corrupt-selftest/zephyros-m5-corrupt.bin",
                  file=sys.stderr)
    elif mode == "m6":
        if M5_CORRUPT_PANIC in data:
            print("serial_smoke: hint: ROMFS init failed with the exact "
                  "m5-corrupt acceptance transcript; the m5-corrupt image "
                  "from the M5 pass is probably still flashed (it rejects "
                  "at the header reserved field, code 0x0A, while the "
                  "staged header still reads ZRFS).  Flash "
                  "build-m6-selftest/zephyros-m6-selftest.bin and rerun "
                  "--mode m6", file=sys.stderr)
        elif M6_FAILURE_OK in data:
            print("serial_smoke: hint: the M6 failure marker was observed; "
                  "the m6-failure image is probably flashed "
                  "(use --mode m6-failure)", file=sys.stderr)
        elif M5_SELFTEST_OK in data:
            print("serial_smoke: hint: the M5 success marker was observed; "
                  "an M5 image may be flashed instead of "
                  "build-m6-selftest/zephyros-m6-selftest.bin",
                  file=sys.stderr)
    elif mode == "m6-failure":
        if M5_CORRUPT_PANIC in data:
            print("serial_smoke: hint: ROMFS init failed with the exact "
                  "m5-corrupt acceptance transcript; the m5-corrupt image "
                  "from the M5 pass is probably still flashed.  Flash "
                  "build-m6-failure-selftest/zephyros-m6-failure.bin and "
                  "rerun --mode m6-failure", file=sys.stderr)
        elif M6_SELFTEST_OK in data:
            print("serial_smoke: hint: the M6 success marker was observed; "
                  "the M6 success image is probably flashed "
                  "(use --mode m6)", file=sys.stderr)
        elif M5_SELFTEST_OK in data:
            print("serial_smoke: hint: the M5 success marker was observed; "
                  "an M5 image may be flashed instead of "
                  "build-m6-failure-selftest/zephyros-m6-failure.bin",
                  file=sys.stderr)
    elif mode == "m7":
        if M6_SELFTEST_OK in data:
            print("serial_smoke: hint: the M6 success marker was observed; "
                  "flash build-m7-selftest/zephyros-m7-selftest.bin and "
                  "rerun --mode m7", file=sys.stderr)
        elif M5_CORRUPT_PANIC in data:
            print("serial_smoke: hint: ROMFS init failed with the exact "
                  "m5-corrupt acceptance transcript; the m5-corrupt image "
                  "from the M5 pass is probably still flashed.  Flash "
                  "build-m7-selftest/zephyros-m7-selftest.bin and rerun "
                  "--mode m7", file=sys.stderr)
        elif not (BANNER in data and CONSOLE_DYNAMIC_OK in data
                  and M7_SELFTEST_OK in data):
            if BANNER in data:
                print("serial_smoke: hint: the captured boot log is "
                      "incomplete (missing the dynamic console line "
                      "and/or 'm7 selftest: OK'); the capture may have "
                      "started mid-boot or the wrong image is flashed.  "
                      "Start this tool first, then power-cycle the board "
                      "or press RESET while it waits; if it persists, "
                      "flash build-m7-selftest/zephyros-m7-selftest.bin "
                      "and rerun --mode m7", file=sys.stderr)
            else:
                print("serial_smoke: hint: no boot log was captured.  The "
                      "boot lines (M0 banner, dynamic console line, "
                      "'m7 selftest: OK') print once at boot; if the "
                      "board was already running, the FT230X only kept "
                      "the last few dozen bytes.  Start this tool first, "
                      "then power-cycle the board or press RESET while "
                      "it waits, and rerun", file=sys.stderr)
        elif M7_BANNER in data and data.count(M7_EXIT) < 2:
            print("serial_smoke: hint: the monitor started but the "
                  "scripted session did not complete; the captured "
                  "transcript above shows how far it got.  The script "
                  "waits for the prompt before each paced command, so a "
                  "truncated command echo means the console link is "
                  "dropping bytes (check cabling/flow control)",
                  file=sys.stderr)
    elif mode == "m8-ioc":
        if M8IOC_UNAVAILABLE in data or M8IOC_FAIL in data:
            print("serial_smoke: hint: the IOC selftest did not complete "
                  "(the module rejected the link, the transport failed, "
                  "or the /SHARED check failed).  Check the IOC MCU is "
                  "running, the command and bulk lane wiring, the card "
                  "and /SHARED folder, and that "
                  "build-m8-ioc-selftest/"
                  "zephyros-m8-ioc-selftest.bin is flashed",
                  file=sys.stderr)
        elif M7_SELFTEST_OK in data:
            print("serial_smoke: hint: the M7 marker was observed; the M7 "
                  "image is probably flashed instead of "
                  "build-m8-ioc-selftest/"
                  "zephyros-m8-ioc-selftest.bin", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        elif M8IOC_SELFTEST_PENDING_RE.search(data) is not None:
            print("serial_smoke: hint: the transcript is a PENDING run "
                  "(the deployed firmware has no writable /SHARED "
                  "service); if this is unexpected, check the IOC MCU "
                  "firmware level and that CMD_FS_OPEN is served",
                  file=sys.stderr)
        elif M8IOC_SELFTEST_OK in data and not _m8ioc_complete(data):
            print("serial_smoke: hint: the selftest line was observed but "
                  "the bulk/cleanup markers are incomplete; the "
                  "transcript above shows how far it got", file=sys.stderr)
    elif mode == "m8-hid":
        found_typed = len(M8HID_RX_RE.findall(data))
        if M8HID_UNAVAILABLE in data or M8HID_FAIL in data:
            print("serial_smoke: hint: the HID selftest did not complete "
                  "(HID is unavailable or the typing/fallback phase "
                  "failed).  Check the IOC MCU is running, the USB "
                  "keyboard is attached, and that "
                  "build-m8-hid-selftest/"
                  "zephyros-m8-hid-selftest.bin is flashed; the "
                  "transcript above shows how far it got",
                  file=sys.stderr)
        elif M8IOC_SELFTEST_OK in data:
            print("serial_smoke: hint: the M8 IOC marker was observed; "
                  "the IOC image is probably flashed instead of "
                  "build-m8-hid-selftest/"
                  "zephyros-m8-hid-selftest.bin", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        elif found_typed < len(M8HID_TYPED_BYTES):
            print("serial_smoke: hint: the USB-keyboard pass is "
                  "incomplete: %d of %d scripted bytes were echoed.  "
                  "Type exactly a B 3 Enter Backspace Ctrl-X Arrow-Up "
                  "when the prompt appears (the bytes are echoed as "
                  "'m8 hid: rx XX' lines)"
                  % (found_typed, len(M8HID_TYPED_BYTES)), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
