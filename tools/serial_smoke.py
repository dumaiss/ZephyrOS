#!/usr/bin/env python3
"""ZephyrOS physical-hardware serial smoke capture.

Opens the SIO0/B serial link (115200 8N1, raw) on the given device,
captures output with a bounded timeout, and verifies the expected output:

    --mode normal    the production boot contract: the banner, the
                     console-service line, the monitor banner and its
                     prompt (optional-module diagnostics tolerated)
    --mode subsystem  the subsystem acceptance image: the banner, the
                     console-service line, the four mechanism groups
                     (`syscall selftest: OK`, `irq selftest: OK`,
                     `romfs selftest: OK`, `process selftest: OK`) and
                     no FAIL/PANIC marker, then the interactive monitor
                     step: when `Z80> ` appears the tool sends `Q` over
                     the link and the mode finishes on `monitor: exit
                     status 0x`.  The boot log is printed once, so start
                     this tool first and reset the board.
    --mode console-integration  the console integration acceptance image:
                     banner, the service report, the console version,
                     the selected output/input lanes, the IOC PING
                     controller level, the rejected-call invariants and
                     "console integration: OK" over the direct early SIO,
                     followed by the IOC, HID and V9958 device
                     diagnostics' markers.  The interactive monitor
                     session runs over the HID input and the V9958
                     output and is the human gate; the image waits for
                     the host sync sequence.
    --mode console-serial  the console-serial variant image (work package
                     42e): the production service set with the SERIAL
                     console, so the whole dynamic transcript - banner,
                     `console: service OK`, the monitor banner and prompt -
                     is on the serial lane.  The tool types `r` at the
                     prompt and checks the monitor's system row comes back
                     on the same lane.  The image waits for the host sync
                     sequence; start this tool first and reset the board.
    --mode fs        the filesystem outcome image: the banner, one OK
                     line per outcome, the child's `fs-child: PASS`, the
                     `fs-selftest: PASS` verdict with its check count and
                     then the monitor prompt on the same SIO console.
    --mode hwtest    the production image's feature exerciser (work
                     package 45b): the boot banner, then the tool types
                     `X /SYSTEM/hwtest.zex` at the monitor over this lane
                     - production's console input is the HID build, so
                     the monitor reads commands from the lane the runner
                     mirrors its lines to - and checks the three stage
                     lines and the verdict, with the card state the
                     transcript reports (insert a card for the card run,
                     remove it for the absent-card run).
All new subsystem modes send the host sync sequence while the banner is
pending, because the acceptance builds hold the first byte until the
host is listening (the bench reset also resets the USB bridge).

The device is given with --port or the ZOS_SERIAL_PORT environment
variable (the SERIAL_PORT make variable is passed through).  A missing or
unopenable device fails clearly; ordinary host-only `make test` never
invokes this tool.

On benches where the target's reset line also resets the USB bridge, the
TTY drops off the bus when the board is reset.  The tool reopens the same
device path and keeps capturing.  The IOC/HID/V9958/console smoke
images additionally wait for the host sync sequence (IOC_HOST_SYNC)
before they print anything, so those modes send that sequence once the
port is open and repeat it until the first byte arrives.

Usage:
    serial_smoke.py --port /dev/ttyUSB0 [--mode normal|subsystem|romfs-corrupt|ioc|hid|video|console|console-integration|console-serial|fs|hwtest]
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
)
CONSOLE_SERVICE_OK = b"console: service OK\r\n"

# --- subsystem acceptance image ---------------------------------------------
# The subsystem image exercises every retained mechanism group on the
# current VFS/ROMFS boot path in one pass.  The transcript constants are
# the group markers the target code prints; the process markers are
# emitted by tests/target/process/ (process_test.c and the zextest ZEX
# program it runs).
SUBSYS_SYSCALL_OK = b"syscall selftest: OK\r\n"
SUBSYS_IRQ_OK = b"irq selftest: OK\r\n"
SUBSYS_ROMFS_OK = b"romfs selftest: OK\r\n"
SUBSYS_PROCESS_OK = b"process selftest: OK\r\n"
SUBSYS_FAIL = b"selftest: FAIL"
SUBSYS_PROMPT = b"Z80> "
MONITOR_BANNER = b"ZephyrOS native monitor"

# Production boot contract: the console service must bind and the monitor
# must reach its prompt.  Optional boot-module diagnostics ("<name>:
# unavailable 0x..") are tolerated -- a bench without the IOC/HID/V9958
# hardware legitimately falls back to SIO -- but a failed console bind or
# an unbound console is a failure.
PRODUCTION_CONSOLE_FAIL = (b"console: unbound\r\n",
                           b"console: service failed\r\n")
# A failed optional boot module prints "<name>: unavailable 0x<code>"; the
# production boot set must be complete, so this is a failure in the
# banner-only contract too.
MODULE_UNAVAILABLE = b": unavailable 0x"

# The card was absent, write-protected or unreadable: the acceptance image
# takes that as a documented outcome rather than a failure.
SUBSYS_EXIT = b"monitor: exit status 0x"

# --- corrupt-store image -----------------------------------------------------
INTEGRATION_SERVICES_PREFIX = b"console integration: services "
INTEGRATION_LANES_RE = re.compile(
    rb"console integration: (output (video|serial)|input (hid|serial))\r\n")
INTEGRATION_IOC_LEVEL_OK = b"console integration: ioc level 4A\r\n"
INTEGRATION_REJECTED_OK = b"console integration: rejected calls OK\r\n"
INTEGRATION_SELFTEST_OK = b"console integration: OK\r\n"
INTEGRATION_FAIL = b"console integration: FAIL"
# The three device diagnostics the integration image carries (the same
# checks the retired ioc/hid/video images ran; their transcripts follow
# the integration one on the same early-SIO lane).
IOC_SELFTEST_OK = b"ioc selftest: OK\r\n"
HID_SELFTEST_OK = b"hid selftest: OK\r\n"
VIDEO_SELFTEST_OK = b"video selftest: OK\r\n"
DEVICE_FAIL_ANY = (b"ioc selftest: FAIL", b"hid selftest: FAIL",
                   b"video selftest: FAIL")
CONSOLE_SERIAL_SYSINFO = \
    b"system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02\r\n"


def _serial_result(data):
    """True when the console-serial variant's transcript is complete: the
    boot banner, the console-service line, the monitor banner and prompt,
    and the system row the mode typed `r` for - all on the serial lane."""
    if b"PANIC" in data or any(marker in data
                               for marker in PRODUCTION_CONSOLE_FAIL):
        return False
    return (BANNER in data and CONSOLE_SERVICE_OK in data and
            MONITOR_BANNER in data and SUBSYS_PROMPT in data and
            CONSOLE_SERIAL_SYSINFO in data)


def _production_failure(data):
    """True when the production boot reported a fatal condition."""
    return (b"PANIC" in data
            or any(marker in data for marker in PRODUCTION_CONSOLE_FAIL)
            or MODULE_UNAVAILABLE in data)


def _production_strong(data):
    """True when the serial console printed the console/monitor lines.

    This is the stronger contract: the console-serial variant routes the
    dynamic console to this lane, so it prints the console-service line,
    the monitor banner and the prompt."""
    return all(needle in data for needle in
               (BANNER, CONSOLE_SERVICE_OK, MONITOR_BANNER, SUBSYS_PROMPT))


def _production_result(data, quiet_elapsed):
    """The production boot contract.

    On real hardware the production console binds to the V9958, so serial
    carries only the 65-byte early banner.  PASS when the four banner lines
    arrive in order and no failure marker follows within the quiet window.
    If the console/monitor lines do reach serial (the console-serial
    variant), the stronger check applies."""
    if _production_failure(data):
        return False
    if _production_strong(data):
        return True
    return BANNER in data and quiet_elapsed


def _integration_result(data):
    """True when the integration image's serial portion completed: the
    service report, the console composition, the PING level, the
    rejected-call invariants and the three device diagnostics, with no
    optional-module diagnostic and no failure marker."""
    if (INTEGRATION_FAIL in data or b"PANIC" in data or
            any(marker in data for marker in DEVICE_FAIL_ANY)):
        return False
    if (BANNER not in data or INTEGRATION_SERVICES_PREFIX not in data or
            INTEGRATION_IOC_LEVEL_OK not in data or
            INTEGRATION_REJECTED_OK not in data or
            INTEGRATION_SELFTEST_OK not in data):
        return False
    lanes = INTEGRATION_LANES_RE.findall(data)
    if len(lanes) != 2:
        return False
    return (IOC_SELFTEST_OK in data and HID_SELFTEST_OK in data and
            VIDEO_SELFTEST_OK in data)


# Host sync the smoke image waits for before the banner (bench quirk: the
# board's reset line also resets the USB bridge, so the board must not
# print until the host has reopened its port).  Mirrors
# ZOS_SMOKE_HOST_SYNC_FIRST/SECOND in platform/zephyr80/hardware.{h,inc};
# modes ioc and hid send it on every open and repeat it until the
# first byte.
IOC_HOST_SYNC = b"\xaa\x55"


def _subsys_result(data):
    """True when the subsystem image's serial sections completed: the
    banner, the console-service line and the four mechanism-group
    markers, with no FAIL/PANIC marker anywhere in the transcript."""
    if SUBSYS_FAIL in data or b"PANIC" in data:
        return False
    for needle in (BANNER, CONSOLE_SERVICE_OK, SUBSYS_SYSCALL_OK,
                   SUBSYS_IRQ_OK, SUBSYS_ROMFS_OK, SUBSYS_PROCESS_OK):
        if needle not in data:
            return False
    return True


def _subsys_complete(data):
    """The serial portion plus the interactive monitor step: the mode
    sends `Q` at the prompt and waits for the monitor exit line."""
    return _subsys_result(data) and SUBSYS_EXIT in data





# Package 42b (correction 01): the filesystem outcome image.  Production
# boot path with `/FS` served by the IOC-FAT service, one SDK outcome
# program and the monitor it hands the console to.  A writable card must
# pass every outcome; a missing or write-protected card must take the
# card-absent branch, keep `/SYSTEM` usable and still reach the verdict.
FS_IMAGE_PASS = b"fs-selftest: PASS\r\n"
FS_IMAGE_FAIL = b"fs-selftest: FAIL"
FS_IMAGE_ABSENT = b"fs-selftest: card absent: /FS not served"
FS_IMAGE_READONLY = b"fs-selftest: card not writable"
FS_IMAGE_MONITOR = b"Z80> "
FS_CHILD_PASS = b"fs-child: PASS\r\n"


def _fs_image_result(data):
    """True when the filesystem outcome image reported success: the boot
    banner, the child's own stdout, the program's verdict and then the
    monitor prompt on the same SIO console - with no FAIL/PANIC marker."""
    if (FS_IMAGE_FAIL in data or b"PANIC" in data or
            b"fs-child: FAIL" in data or FS_CHILD_PASS not in data):
        return False
    return (BANNER in data and FS_IMAGE_PASS in data and
            FS_IMAGE_MONITOR in data)


# --- package 45b: the production hardware exerciser --------------------------
# `X /SYSTEM/hwtest.zex` on the production image.  The console is the
# V9958/HID build there, so this lane is where the monitor takes its
# commands (it polls the SERIAL object beside the console) and where the
# runner mirrors every line - the tool therefore reads its own command
# echoed back byte by byte and then the stage lines and the verdict.
HW_COMMAND = b"X /SYSTEM/hwtest.zex\r"
HW_TIMEOUT = 120.0
HW_BEGIN = b"hwtest: begin\r\n"
HW_STAGES = (b"hwtest: xmem+far PASS\r\n",
             b"hwtest: namespace+handles PASS\r\n")
HW_FILES_PASS = b"hwtest: files PASS\r\n"
HW_FILES_ABSENT = b"hwtest: files card absent\r\n"
HW_FILES_READONLY = b"hwtest: files card not writable\r\n"
HW_SUMMARY = b"hwtest: 3/3 PASS"
# The card state each line pair reports: the files stage and the note the
# verdict carries.
HW_CARD_STATES = (
    ("present", HW_FILES_PASS, b""),
    ("absent", HW_FILES_ABSENT, b" (card absent)"),
    ("not writable", HW_FILES_READONLY, b" (card not writable)"),
)
HW_FAIL = b"hwtest: 3/3 FAIL"

# The quiet window after the boot banner before the command goes in: the
# monitor's launcher, console service and poll loop all run behind the
# banner, which the early console prints first.
HW_SETTLE = 1.5


def _hwtest_card(data):
    """The card state the exerciser's transcript reports, or None while it
    is incomplete.  A state counts only when the begin line, both program
    stages, the files line and the matching verdict are all present, so a
    partial or inconsistent transcript never reads as a pass."""
    if HW_BEGIN not in data:
        return None
    for state, files_line, note in HW_CARD_STATES:
        if (files_line in data and
                HW_SUMMARY + note + b"\r\n" in data and
                all(marker in data for marker in HW_STAGES)):
            return state
    return None


def _hwtest_result(data):
    """True when the exerciser completed on this lane: the three stage
    lines and the verdict, with no FAIL/PANIC marker."""
    if HW_FAIL in data or b"PANIC" in data or b" FAIL" in data:
        return False
    return _hwtest_card(data) is not None


class HwtestCommand:
    """The exerciser command's typed-speed handshake.

    The monitor echoes every byte it takes from this lane back on the same
    lane (the input path of work package 45b), which turns the send into a
    checked one: each byte of `HW_COMMAND` goes out only after the previous
    byte's echo arrived, so the polled receiver's single-byte register
    never sees a burst, and a byte whose echo does not arrive inside the
    deadline is sent again - it replaces the pending byte rather than
    duplicating it.  A line that was acknowledged but produced no `hwtest:`
    line (the monitor may have executed a garbled one) is restarted behind
    a flush CR, so a partial line can never join the new one.
    """

    ECHO_DEADLINE = 1.0
    PACE = 0.02
    BEGIN_DEADLINE = 8.0
    RESTARTS = 4

    def __init__(self):
        self.next = 0          # index of the byte to send next
        self.outstanding = -1  # index that is waiting for its echo
        self.mark = 0          # `data` offset when it was written
        self.deadline = 0.0
        self.ack_at = 0.0
        self.restarts = 0
        self.done = False
        self.gave_up = False

    def _write(self, link, payload, data_len, now):
        try:
            link.write(payload)
            link.flush()
        except Exception:
            return
        self.outstanding = self.next
        self.mark = data_len
        self.deadline = now + self.ECHO_DEADLINE

    def _restart(self):
        self.next = 0
        self.outstanding = -1
        self.done = False
        self.deadline = 0.0
        self.ack_at = 0.0
        self.restarts += 1

    def step(self, link, data, now):
        """Advance the handshake; returns a note string when it acts."""
        if self.gave_up:
            return None
        if self.done:
            if HW_BEGIN in data or now < self.ack_at + self.BEGIN_DEADLINE:
                return None
            if self.restarts >= self.RESTARTS:
                self.gave_up = True
                return ("the command was echoed %d times but no stage line "
                        "followed" % self.restarts)
            self._restart()
            return ("no stage line followed the acknowledged command: "
                    "restarting it")
        if self.outstanding < 0:
            if now < self.deadline:
                return None  # pacing (or the flush CR before a restart)
            if self.next == 0 and self.restarts != 0:
                self._write(link, b"\r", len(data), now)
                self.outstanding = -1
                self.deadline = now + self.PACE
                return "flushed a partial line before restarting"
            self._write(link, HW_COMMAND[self.next:self.next + 1], len(data),
                        now)
            if self.next == 0:
                return "typed %r at the monitor prompt" % HW_COMMAND
            return None
        if HW_COMMAND[self.outstanding] in data[self.mark:]:
            self.next = self.outstanding + 1
            self.outstanding = -1
            self.deadline = now + self.PACE
            if self.next >= len(HW_COMMAND):
                self.done = True
                self.ack_at = now
                return "the monitor acknowledged the command"
            return None
        if now >= self.deadline:
            self.outstanding = -1
            self.deadline = now + self.PACE
            return "byte %d was not echoed: sending it again" % self.next
        return None


def _hwtest_complete(data):
    """True once the exerciser's verdict arrived (or it failed) on the
    lane."""
    return _hwtest_card(data) is not None or HW_FAIL in data


def main(argv):
    parser = argparse.ArgumentParser(description="ZephyrOS serial smoke check")
    parser.add_argument("--port", default=os.environ.get("ZOS_SERIAL_PORT", ""))
    parser.add_argument(
        "--baud", type=int, default=int(os.environ.get("ZOS_SERIAL_BAUD", "115200"))
    )
    parser.add_argument("--mode", choices=("normal", "subsystem",
                                           "console-integration",
                                           "console-serial",
                                           "fs", "hwtest"),
                        default="normal")
    parser.add_argument(
        "--timeout", type=float, default=None,
        help="capture window in seconds (default: ZOS_SERIAL_TIMEOUT, else "
             "15, or %g for hwtest, which runs the whole exerciser)"
             % HW_TIMEOUT
    )
    parser.add_argument(
        "--quiet", type=float,
        default=float(os.environ.get("ZOS_SERIAL_QUIET", "5")),
        help="normal mode: quiet window after the banner before a "
             "banner-only PASS (default 5s)")
    args = parser.parse_args(argv)

    if args.timeout is None:
        env_timeout = os.environ.get("ZOS_SERIAL_TIMEOUT", "")
        if env_timeout:
            args.timeout = float(env_timeout)
        else:
            args.timeout = HW_TIMEOUT if args.mode == "hwtest" else 15.0

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
    if args.mode == "subsystem":
        print("serial_smoke: mode subsystem checks the boot log and the "
              "interactive monitor step; power-cycle the board or press "
              "RESET now so the boot is captured; the tool sends Q once "
              "the Z80> prompt appears and finishes on the monitor exit "
              "line")
    if args.mode == "console-integration":
        print("serial_smoke: mode console-integration checks the integration image's "
              "serial portion (service report, composition lanes, PING "
              "level, rejected-call invariants, the IOC/HID/V9958 "
              "diagnostics); power-cycle the board or press RESET now so "
              "the boot is captured; the image waits for the host sync "
              "sequence")
        print("serial_smoke: operator step: type 'K41' then Enter on the "
              "USB keyboard when the console lanes are reported - the "
              "console composition read waits for it and reports the "
              "typed line back")
        print("serial_smoke: the interactive monitor session runs over "
              "the HID input and the V9958 output; confirm the banner, "
              "prompt and documented command pass on the physical "
              "keyboard/display")
    if args.mode == "console-serial":
        print("serial_smoke: mode console-serial checks the console-serial "
              "variant boot log; power-cycle the board or press RESET now "
              "so the boot is captured; the image waits for the host sync "
              "sequence")
        print("serial_smoke: the dynamic console is the serial lane here; "
              "this tool types 'r' at the monitor prompt and requires the "
              "monitor's system row on the same lane")
    if args.mode == "fs":
        print("serial_smoke: mode fs checks the filesystem outcome image "
              "log; power-cycle the board or press RESET now so the boot "
              "is captured; with a writable card every outcome line and "
              "the fs-selftest: PASS verdict must appear")
    if args.mode == "hwtest":
        print("serial_smoke: mode hwtest checks the production image's "
              "feature exerciser; power-cycle the board or press RESET "
              "now so the boot is captured; the tool then types "
              "'X /SYSTEM/hwtest.zex' at the monitor over this lane (the "
              "production console input is the HID build, so the monitor "
              "reads its commands here) and reads the stage lines and the "
              "verdict")
        print("serial_smoke: operator step: run this twice - once with a "
              "FAT card inserted and once without it; the files line and "
              "the verdict carry the card state, and with no card the "
              "expected result is 'card absent'")
    deadline = time.monotonic() + args.timeout
    data = b""
    bridge_drops = 0
    host_sync_at = 0.0
    rx_byte_sent = False
    normal_banner_at = 0.0
    hwtest_cmd = HwtestCommand()
    hwtest_idle_at = 0.0
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
            if (args.mode in ("console-integration",
                              "console-serial") and
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
                    link.write(IOC_HOST_SYNC)
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
                if args.mode == "subsystem":
                    # The monitor step is interactive: send Q once the
                    # prompt appears, then finish on the exit line (the
                    # launcher relaunches the monitor after a normal exit,
                    # so wait only for the first exit line).
                    if SUBSYS_PROMPT in data and not rx_byte_sent:
                        link.write(b"Q")
                        link.flush()
                        rx_byte_sent = True
                        print("serial_smoke: sent Q at the monitor prompt")
                    if _subsys_complete(data):
                        break
                elif args.mode == "console-integration":
                    if _integration_result(data) or INTEGRATION_FAIL in data:
                        break
                elif args.mode == "fs":
                    # Read past the verdict: the monitor prompt is part of
                    # the contract for this image.
                    if _fs_image_result(data) or FS_IMAGE_FAIL in data or \
                            b"fs-child: FAIL" in data:
                        break
                elif args.mode == "console-serial":
                    # The dynamic console is the serial lane: type `r` at
                    # the prompt and finish on the monitor's system row.
                    if SUBSYS_PROMPT in data and not rx_byte_sent:
                        try:
                            link.write(b"r\r")
                            link.flush()
                        except (serial.SerialException, OSError):
                            pass  # the read notices a dropped link
                        rx_byte_sent = True
                        print("serial_smoke: sent 'r' at the monitor prompt")
                    if _serial_result(data):
                        break
                elif args.mode == "hwtest":
                    if _hwtest_complete(data):
                        break
            if args.mode == "hwtest" and link is not None:
                # The exerciser's command handshake starts once the boot
                # banner is complete and the lane has been quiet: the
                # monitor reaches its command loop right behind the banner.
                if BANNER in data:
                    now = time.monotonic()
                    if hwtest_idle_at == 0.0:
                        hwtest_idle_at = now + HW_SETTLE
                        print("serial_smoke: boot captured; typing the "
                              "command in over the lane once it is quiet")
                    if now >= hwtest_idle_at:
                        note = hwtest_cmd.step(link, data, now)
                        if note:
                            print("serial_smoke: " + note)
            if args.mode == "normal":
                # Banner-only contract: the V9958-bound production console
                # sends only the early banner on serial.  PASS once the
                # banner is complete and the quiet window elapsed with no
                # failure marker; the console-serial variant passes early
                # on the stronger console/monitor evidence.
                if BANNER in data and normal_banner_at == 0.0:
                    normal_banner_at = time.monotonic()
                if _production_failure(data) or _production_strong(data):
                    break
                if normal_banner_at != 0.0 and (
                        time.monotonic() - normal_banner_at >= args.quiet):
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
        quiet_elapsed = (normal_banner_at != 0.0 and
                         time.monotonic() - normal_banner_at >= args.quiet) \
            or _production_strong(data) or _production_failure(data)
        ok = _production_result(data, quiet_elapsed)
        expected = ("the four banner lines in order and a quiet %gs window "
                    "with no PANIC, no failed console bind and no "
                    "'<module>: unavailable' line; or, on the console-serial "
                    "variant, 'console: service OK', the monitor "
                    "banner and the 'Z80> ' prompt" % args.quiet)
        if ok and not _production_strong(data):
            print("serial_smoke: banner-only PASS - the production console "
                  "is bound to the V9958, so serial carries only the banner")
            print("serial_smoke: operator step: confirm on the V9958 the "
                  "monitor banner, the status line and the 'Z80>' prompt, "
                  "then type a key")
    elif args.mode == "subsystem":
        ok = _subsys_complete(data)
        expected = ("the banner, the console-service line, the four "
                    "mechanism groups (syscall/irq/romfs/process "
                    "selftest: OK) with no FAIL/PANIC marker, and one "
                    "interactive monitor step ending on 'monitor: exit "
                    "status 0x'")
    elif args.mode == "console-integration":
        ok = _integration_result(data)
        expected = ("the banner, the service report, the console "
                    "composition lanes, the typed 'K41' line, the IOC "
                    "PING level, the rejected-call invariants, the "
                    "IOC/HID/V9958 device diagnostics and "
                    "'console integration: OK'; the operator must also "
                    "confirm the visible monitor session on the V9958 "
                    "display with the USB keyboard")
    elif args.mode == "fs":
        ok = _fs_image_result(data)
        expected = ("the banner, one OK line per outcome (the /FS create, "
                    "write, read, seek, enumerate, mkdir, unlink and rename "
                    "checks, the /SYSTEM read and the nested exec), the "
                    "child's 'fs-child: PASS' with its leaked handles "
                    "released, 'fs-selftest: PASS' with the exact check "
                    "count, and then the monitor prompt 'Z80> ' on the same "
                    "SIO console - with a writable FAT card and no "
                    "card-absent diagnostic")
    elif args.mode == "console-serial":
        ok = _serial_result(data)
        expected = ("the banner, 'console: service OK', the monitor banner "
                    "and the 'Z80> ' prompt, and the monitor's system row "
                    "for the typed 'r' - all on the serial lane (the "
                    "console-serial variant routes /dev/console to SERIAL)")
    elif args.mode == "hwtest":
        ok = _hwtest_result(data)
        expected = ("the boot banner, the echoed command, and the "
                    "exerciser's transcript on this lane: 'hwtest: begin', "
                    "'hwtest: xmem+far PASS', the files stage line (PASS, "
                    "'card absent' or 'card not writable'), "
                    "'hwtest: namespace+handles PASS' and "
                    "'hwtest: 3/3 PASS' with the card note that matches the "
                    "files line - run it once with a card and once without")

    _root_observation(args.mode, data)
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


def _root_observation(mode, data):
    """Report which state the optional `/` mount reached.

    Both production and the IOC-FAT acceptance image answer this on the
    early console, so it is always in the serial capture; without it the
    two card states are indistinguishable from the transcript alone.
    """
    if FS_IMAGE_ABSENT in data or FS_IMAGE_READONLY in data:
        print("serial_smoke: note: /FS is not served (no card, or a "
              "write-protected one); this image reports that as an "
              "expected outcome")
    state = _hwtest_card(data) if mode == "hwtest" else None
    if state is not None:
        print("serial_smoke: note: the exerciser reports card state '%s'"
              % state)



def _explain_wrong_image(mode, data):
    """Turn an image/mode mismatch into an actionable hint.

    Each acceptance image is flashed separately; a run with the wrong
    --mode otherwise fails with an unexplained timeout on the wrong
    transcript.
    """
    if mode == "subsystem":
        if not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  The "
                  "boot lines (banner and group markers) print once at "
                  "boot; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        elif not _subsys_result(data):
            print("serial_smoke: hint: the captured boot log is "
                  "incomplete (one or more mechanism groups did not "
                  "report OK); the capture may have started mid-boot or "
                  "the wrong image is flashed", file=sys.stderr)
        else:
            print("serial_smoke: hint: the groups completed but the "
                  "interactive monitor step did not finish; the "
                  "transcript above shows how far it got (the tool sends "
                  "Q at the Z80> prompt)", file=sys.stderr)
    elif mode == "normal":
        if b"PANIC" in data or any(marker in data
                                   for marker in PRODUCTION_CONSOLE_FAIL):
            print("serial_smoke: hint: the production boot reported a "
                  "panic or a console failure (see the marker in the "
                  "transcript above)", file=sys.stderr)
        elif BANNER in data and CONSOLE_SERVICE_OK not in data:
            print("serial_smoke: hint: only the early serial banner was "
                  "captured.  The production image binds its console to "
                  "the V9958 output, so 'console: service OK', the "
                  "monitor banner and the 'Z80> ' prompt are rendered on "
                  "the V9958 display and never reach the SIO port; "
                  "confirm the monitor prompt and HID input on the V9958 "
                  "display", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
    elif mode == "fs":
        if FS_IMAGE_FAIL in data:
            print("serial_smoke: hint: an outcome failed on a mounted card; "
                  "record the FAIL label and the card state in the "
                  "package record", file=sys.stderr)
        elif FS_IMAGE_ABSENT in data or FS_IMAGE_READONLY in data:
            print("serial_smoke: hint: /FS is not served, so this image took "
                  "its card-absent branch; the mounted-card gate needs a "
                  "writable FAT card inserted", file=sys.stderr)
        elif b"fs-child: FAIL" in data:
            print("serial_smoke: hint: the nested-exec child failed; the "
                  "tail, the child's own stdout or its exit status did not "
                  "survive the exec", file=sys.stderr)
        elif BANNER not in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
    elif mode == "console-integration":
        if INTEGRATION_FAIL in data or any(marker in data
                                           for marker in DEVICE_FAIL_ANY):
            print("serial_smoke: hint: a diagnostic reported FAIL; the "
                  "marker above names the phase", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        else:
            print("serial_smoke: hint: the integration transcript is "
                  "incomplete; the last marker above shows how far the "
                  "boot got.  The interactive monitor session must be "
                  "confirmed on the physical keyboard and display",
                  file=sys.stderr)
    elif mode == "console-serial":
        if not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        else:
            print("serial_smoke: hint: the serial-console transcript is "
                  "incomplete; the last marker above shows how far the "
                  "boot got.  The dynamic console must answer on this "
                  "lane: check the console-service line and the monitor "
                  "prompt above", file=sys.stderr)
    elif mode == "hwtest":
        if b"PANIC" in data or b"console: unbound" in data:
            print("serial_smoke: hint: the production boot reported a "
                  "panic or an unbound console (see the marker above)",
                  file=sys.stderr)
        elif HW_FAIL in data or b" FAIL" in data:
            print("serial_smoke: hint: an exerciser stage failed; the "
                  "stage line above names it (a stage program reports its "
                  "own FAIL lines on the console, i.e. the V9958 "
                  "display)", file=sys.stderr)
        elif BANNER not in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
        elif HW_BEGIN not in data:
            print("serial_smoke: hint: the monitor did not run the "
                  "command.  The production console input is the HID "
                  "build, so the monitor takes it from this lane; confirm "
                  "the flashed image is the package-45b production image "
                  "(its monitor polls /dev/serial/0) and that the store "
                  "carries /SYSTEM/hwtest.zex", file=sys.stderr)
        else:
            print("serial_smoke: hint: the exerciser started but its "
                  "verdict did not arrive inside the window; raise "
                  "--timeout, and check the console (V9958) for the stage "
                  "programs' own outcome lines", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
