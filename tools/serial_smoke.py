#!/usr/bin/env python3
"""ZephyrOS physical-hardware serial smoke capture.

Opens the SIO0/B serial link (115200 8N1, raw) on the given device,
captures output with a bounded timeout, and verifies the expected output:

    --mode normal    the production boot contract: the exact banner, the
                     dynamic console line, the monitor banner and its
                     prompt (optional-module diagnostics tolerated)
    --mode subsystem  the subsystem acceptance image: the banner, the
                     dynamic console line, the five mechanism groups
                     (`syscall selftest: OK`, `irq selftest: OK`,
                     `romfs selftest: OK`, `process selftest: OK`,
                     `loader selftest: OK`) and no FAIL/PANIC marker,
                     then the interactive monitor step: when `Z80> `
                     appears the tool sends `Q` over the link and the
                     mode finishes on `monitor: exit status 0x`.  The
                     boot log is printed once, so start this tool first
                     and reset the board.
    --mode romfs-corrupt  the corrupt-store image: the banner and the
                     early-console panic marker `PANIC: romfs: corrupt
                     recovery image` with no module execution.  Passes
                     only when that panic is present.
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
    --mode m8-v9958  the M8 V9958 video image: banner, the v9958.0
                     VIDEO registration, the malformed-request
                     rejection, the RESET pass, the console-binding
                     line, the presented fill, the drawn pattern, and
                     "m8 v9958 selftest: OK".  The tool validates the
                     serial transcript; the operator must also confirm
                     the documented pattern on the V9958 output (a
                     white 64x64 rectangle, a white-then-magenta band,
                     default blue background).  The image waits for the
                     host sync sequence; start this tool first and
                     reset the board.
    --mode m8-console  the M8 console acceptance image: banner, the
                     selected input/output class lines
                     (HID keyboard input, V9958 output) and
                     "m8 console selftest: OK".  The interactive
                     session runs over the HID input and the V9958
                     output; the operator must confirm it on the
                     physical keyboard and display.  The image waits
                     for the host sync sequence; start this tool first
                     and reset the board.
    --mode m8        the M8 integration acceptance image: banner, the
                     binding report (hid.keyboard0 input class 04,
                     v9958.0 output class 02), the device count, the
                     controller level returned by PING, the
                     malformed-call invariants and "m8 selftest: OK"
                     over the direct early SIO.  The interactive
                     monitor session runs over the HID input and the
                     V9958 output and is the human gate; the image
                     waits for the host sync sequence.
    --mode m8-fb-nohid  the fallback image with no hid.zmod in the
                     recovery store: `hid: unavailable 0x05`, input
                     falls back to sio0.b, output stays v9958.0; the
                     tool sends the invited input byte and the operator
                     confirms the console marker on the display.
    --mode m8-fb-nohid-nov9958  HID and V9958 both absent:
                     `hid: unavailable 0x05` and `v9958: unavailable
                     0x05`; the console binds SIO/SIO, so both the
                     invited input byte and the console marker are
                     verified in this serial transcript.
    --mode m8-fb-badioc  the malformed-IOC fallback: the fixture IOC
                     (null command operation) fails HID init
                     (`hid: unavailable 0x0B`) while V9958 still
                     loads; the console binds SIO/V9958 (input byte
                     scripted, output marker on the display).
    --mode m8-fb-arena  the arena-exhaustion fallback: the v9958.zmod
                     entry is an oversized allocation (`v9958:
                     unavailable 0x06`); HID stays the input, SIO the
                     output, so the console marker is verified in this
                     serial transcript.

All new M8 modes send the host sync sequence while the banner is
pending, because the acceptance builds hold the first byte until the
host is listening (the bench reset also resets the USB bridge).

The device is given with --port or the ZOS_SERIAL_PORT environment
variable (the SERIAL_PORT make variable is passed through).  A missing or
unopenable device fails clearly; ordinary host-only `make test` never
invokes this tool.

On benches where the target's reset line also resets the USB bridge, the
TTY drops off the bus when the board is reset.  The tool reopens the same
device path and keeps capturing.  The M8 IOC/HID/V9958/console smoke
images additionally wait for the host sync sequence (M8IOC_HOST_SYNC)
before they print anything, so those modes send that sequence once the
port is open and repeat it until the first byte arrives.

Usage:
    serial_smoke.py --port /dev/ttyUSB0 [--mode normal|subsystem|romfs-corrupt|m8-ioc|m8-hid|m8-v9958|m8-console|m8|m8-fb-nohid|m8-fb-nohid-nov9958|m8-fb-badioc|m8-fb-arena|m9-fs|m9-exec|m9-iocfat|m9]
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
CONSOLE_DYNAMIC_OK = b"console: dynamic sio0.b OK\r\n"

# --- subsystem acceptance image ---------------------------------------------
# The subsystem image exercises every retained mechanism group on the
# current VFS/ROMFS boot path in one pass.  The transcript constants are
# the group markers the target code prints; the process markers are
# emitted by tests/target/process/ (process_test.c and the zextest ZEX
# program it runs).  SUBSYS_STATUS_* and SUBSYS_ZEXTEST_BANK* name the
# composed transcript lines (the target prints the prefix and the hex
# digits through separate writes, so only the prefix is a linked literal).
SUBSYS_SYSCALL_OK = b"syscall selftest: OK\r\n"
SUBSYS_IRQ_OK = b"irq selftest: OK\r\n"
SUBSYS_ROMFS_OK = b"romfs selftest: OK\r\n"
SUBSYS_PROCESS_OK = b"process selftest: OK\r\n"
SUBSYS_LOADER_OK = b"loader selftest: OK\r\n"
SUBSYS_STATUS_PREFIX = b"process selftest: zextest status 0x"
SUBSYS_ZEXTEST_BANK0 = b"zextest: bank=0"
SUBSYS_ZEXTEST_BANK1 = b"zextest: bank=1"
SUBSYS_ZEXTEST_OK = b"zextest: OK\r\n"
SUBSYS_STATUS_PASS = b"process selftest: zextest status 0xA500\r\n"
SUBSYS_STATUS_PASS_BANK1 = b"process selftest: zextest status 0xA501\r\n"
SUBSYS_REPEAT_OK = b"process selftest: repeated cycles OK\r\n"
SUBSYS_TWO_BANK_OK = b"process selftest: two-bank OK\r\n"
SUBSYS_RAW_RETURN_OK = b"process selftest: raw return EFAULT\r\n"
SUBSYS_REJECTED_OK = b"process selftest: malformed rejected\r\n"
SUBSYS_CORRUPT_ROLLBACK_OK = \
    b"process selftest: corrupt entry rolled back\r\n"
SUBSYS_PROCESS_DONE = b"process selftest: OK\r\n"
SUBSYS_FAIL = b"selftest: FAIL"
SUBSYS_PROMPT = b"Z80> "
M9FS_PASS = b"fstest: PASS "
M9FS_FAIL = b"fstest: FAIL "
M9FS_LAUNCHER_OK = b"fstest: launcher OK\r\n"
M9FS_SELFTEST_OK = b"m9 fs selftest: OK\r\n"
M9FS_SELFTEST_FAIL = b"m9 fs selftest: FAIL"
MONITOR_BANNER = b"ZephyrOS native monitor"

# Production boot contract: the console service must bind and the monitor
# must reach its prompt.  Optional boot-module diagnostics ("<name>:
# unavailable 0x..") are tolerated -- a bench without the IOC/HID/V9958
# hardware legitimately falls back to SIO -- but a failed console bind or
# an unbound console is a failure.
PRODUCTION_CONSOLE_FAIL = (b"console: unbound\r\n",
                           b"console: dynamic driver failed\r\n",
                           b"console: service failed\r\n")
# A failed optional boot module prints "<name>: unavailable 0x<code>"; the
# production boot set must be complete, so this is a failure in the
# banner-only contract too.
MODULE_UNAVAILABLE = b": unavailable 0x"

# The optional production root: the mount pass reports it on the *early*
# console (SIO), so the serial capture always shows which of the two states
# the boot reached - `/` mounted silently, or this diagnostic.
VFS_UNAVAILABLE = b"vfs: / unavailable 0x"

# The card was absent (or the FAT unreadable) but the acceptance image takes
# that as a documented outcome rather than a failure.
M9IOCFAT_ABSENT = b"m9 iocfat selftest: OK (card absent)"
SUBSYS_EXIT = b"monitor: exit status 0x"

# --- corrupt-store image -----------------------------------------------------
ROMFS_CORRUPT_PANIC = b"PANIC: romfs: corrupt recovery image\r\n"

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
# The precise failure diagnostic is "<name>: unavailable 0x<ZOS_E*>"
# (kernel/main.c kernel_module_unavailable); the prefix is the stable
# part used for wrong-image classification.
M8IOC_UNAVAILABLE = b"ioc: unavailable"

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
M8HID_UNAVAILABLE = b"hid: unavailable"
M8HID_FALLBACK_BYTE = b"R"

# --- M8 V9958 video core -----------------------------------------------------
M8V9958_VIDEO_OK = b"m8 v9958: video0 OK\r\n"
M8V9958_MALFORMED_OK = b"m8 v9958: malformed rejected\r\n"
M8V9958_RESET_OK = b"m8 v9958: reset OK\r\n"
M8V9958_CONSOLE_OK = b"m8 v9958: console v9958.0 OK\r\n"
M8V9958_FILL_OK = b"m8 v9958: fill presented\r\n"
M8V9958_PATTERN_OK = b"m8 v9958: pattern drawn\r\n"
M8V9958_SELFTEST_OK = b"m8 v9958 selftest: OK\r\n"
M8V9958_FAIL = b"m8 v9958 selftest: FAIL"
M8V9958_UNAVAILABLE = b"v9958: unavailable"

# --- M8 console (HID input / V9958 output) ------------------------------------
# The console image's early-SIO transcript reports the selected console
# classes; the interactive session itself runs over the HID input and the
# V9958 output and can only be confirmed on the physical keyboard and
# display (or replayed by tests/test_m8_sim.py).
M8CONSOLE_INPUT_OK = b"m8 console: input hid.keyboard0 class 04\r\n"
M8CONSOLE_OUTPUT_OK = b"m8 console: output v9958.0 class 02\r\n"
M8CONSOLE_SELFTEST_OK = b"m8 console selftest: OK\r\n"
M8CONSOLE_FAIL = b"m8 console selftest: FAIL"

# --- M8 integration acceptance (package 20) ---------------------------------
# The integration image's early-SIO transcript: the binding report, the
# device count, the controller level returned by PING, the rejected-call
# invariants and the final marker.  All optional-module diagnostics stay
# absent (every module loaded); the interactive session runs over the
# HID input / V9958 output and is the human gate.
M8_INPUT_OK = b"m8 selftest: input hid.keyboard0 class 04\r\n"
M8_OUTPUT_OK = b"m8 selftest: output v9958.0 class 02\r\n"
M8_DEVICES_OK = b"m8 selftest: devices 04\r\n"
M8_IOC_LEVEL_OK = b"m8 selftest: ioc level 4A\r\n"
M8_REJECTED_OK = b"m8 selftest: rejected calls OK\r\n"
M8_SELFTEST_OK = b"m8 selftest: OK\r\n"
M8_FAIL = b"m8 selftest: FAIL"
M8_UNAVAILABLE_ANY = b": unavailable 0x"

# --- M8 fallback matrix (package 20) ----------------------------------------
M8FB_INPUT_RE = re.compile(
    rb"m8 fallback: input ([A-Za-z0-9._]+) class ([0-9A-F]{2})\r\n")
M8FB_OUTPUT_RE = re.compile(
    rb"m8 fallback: output ([A-Za-z0-9._]+) class ([0-9A-F]{2})\r\n")
M8FB_DEVICES_RE = re.compile(rb"m8 fallback: devices (\d{1,2})\r\n")
M8FB_SEND_INPUT = b"m8 fallback: send input\r\n"
M8FB_INPUT_BYTE = b"R"
M8FB_INPUT_OK_RE = re.compile(rb"m8 fallback: input OK 0x([0-9A-F]{2})\r\n")
M8FB_INPUT_NONE = b"m8 fallback: input none\r\n"
M8FB_CONSOLE_MARKER = b"m8 fallback: console output\r\n"
M8FB_WRITE_OK = b"m8 fallback: console write OK\r\n"
M8FB_SELFTEST_OK = b"m8 fallback selftest: OK\r\n"
M8FB_FAIL = b"m8 fallback selftest: FAIL"

# Observable contract per failure variant: the bound console pair, the
# exact early-SIO diagnostics (ZOS_E* code), and whether the console
# marker must be visible in the serial transcript (output = SIO) or is
# display-only and must therefore NOT appear on the link (output =
# V9958).  A serial-bound input must show the scripted byte read back
# through the console service.
M8FB_MODES = {
    "m8-fb-nohid": {
        "input": (b"sio0.b", b"01"),
        "output": (b"v9958.0", b"02"),
        "diagnostics": (b"hid: unavailable 0x05\r\n",),
        "serial_marker": False,
    },
    "m8-fb-nohid-nov9958": {
        "input": (b"sio0.b", b"01"),
        "output": (b"sio0.b", b"01"),
        "diagnostics": (b"hid: unavailable 0x05\r\n",
                        b"v9958: unavailable 0x05\r\n"),
        "serial_marker": True,
    },
    "m8-fb-badioc": {
        "input": (b"sio0.b", b"01"),
        "output": (b"v9958.0", b"02"),
        "diagnostics": (b"hid: unavailable 0x0B\r\n",),
        "serial_marker": False,
    },
    "m8-fb-arena": {
        "input": (b"hid.keyboard0", b"04"),
        "output": (b"sio0.b", b"01"),
        "diagnostics": (b"v9958: unavailable 0x06\r\n",),
        "serial_marker": True,
    },
}


def _production_failure(data):
    """True when the production boot reported a fatal condition."""
    return (b"PANIC" in data
            or any(marker in data for marker in PRODUCTION_CONSOLE_FAIL)
            or MODULE_UNAVAILABLE in data)


def _production_strong(data):
    """True when the SIO-fallback bench printed the console/monitor lines.

    This is the stronger contract: a bench whose console falls back to SIO
    prints the console-service line, the monitor banner and the prompt."""
    return all(needle in data for needle in
               (BANNER, CONSOLE_DYNAMIC_OK, MONITOR_BANNER, SUBSYS_PROMPT))


def _production_result(data, quiet_elapsed):
    """The production boot contract.

    On real hardware the production console binds to the V9958, so serial
    carries only the 65-byte early banner.  PASS when the four banner lines
    arrive in order and no failure marker follows within the quiet window.
    If the console/monitor lines do reach serial (an SIO-fallback bench),
    the stronger check applies."""
    if _production_failure(data):
        return False
    if _production_strong(data):
        return True
    return BANNER in data and quiet_elapsed


def _m8_result(data):
    """True when the integration image's serial portion completed: the
    binding report, the PING level, the rejected-call invariants, no
    optional-module diagnostic, no failure marker."""
    if M8_FAIL in data or b"PANIC" in data or M8_UNAVAILABLE_ANY in data:
        return False
    for needle in (BANNER, M8_INPUT_OK, M8_OUTPUT_OK, M8_DEVICES_OK,
                   M8_IOC_LEVEL_OK, M8_REJECTED_OK, M8_SELFTEST_OK):
        if needle not in data:
            return False
    return True


def _m8fb_result(mode, data):
    """True when the variant's observable contract is complete."""
    spec = M8FB_MODES[mode]
    if M8FB_FAIL in data or b"PANIC" in data:
        return False
    for needle in (BANNER, M8FB_WRITE_OK, M8FB_SELFTEST_OK):
        if needle not in data:
            return False
    match = M8FB_INPUT_RE.search(data)
    if match is None or (match.group(1), match.group(2)) != spec["input"]:
        return False
    match = M8FB_OUTPUT_RE.search(data)
    if match is None or (match.group(1), match.group(2)) != spec["output"]:
        return False
    if M8FB_DEVICES_RE.search(data) is None:
        return False
    for needle in spec["diagnostics"]:
        if data.count(needle) != 1:
            return False
    if spec["input"][1] == b"01":
        if M8FB_SEND_INPUT not in data or M8FB_INPUT_NONE in data:
            return False
        match = M8FB_INPUT_OK_RE.search(data)
        if match is None or match.group(1) != b"52":
            return False
    if spec["serial_marker"]:
        if M8FB_CONSOLE_MARKER not in data:
            return False
    elif M8FB_CONSOLE_MARKER in data:
        # The bound output is the V9958 display: a marker on the serial
        # link would contradict the reported binding.
        return False
    return True

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


def _subsys_result(data):
    """True when the subsystem image's serial sections completed: the
    banner, the dynamic console line and the five mechanism-group
    markers, with no FAIL/PANIC marker anywhere in the transcript."""
    if SUBSYS_FAIL in data or b"PANIC" in data:
        return False
    for needle in (BANNER, CONSOLE_DYNAMIC_OK, SUBSYS_SYSCALL_OK,
                   SUBSYS_IRQ_OK, SUBSYS_ROMFS_OK, SUBSYS_PROCESS_DONE,
                   SUBSYS_LOADER_OK):
        if needle not in data:
            return False
    return True


def _subsys_complete(data):
    """The serial portion plus the interactive monitor step: the mode
    sends `Q` at the prompt and waits for the monitor exit line."""
    return _subsys_result(data) and SUBSYS_EXIT in data


def _romfs_corrupt_result(data):
    """True only for the corrupt-store image: the early-console panic
    marker is present before any module runs."""
    return BANNER in data and ROMFS_CORRUPT_PANIC in data


def _m8v9958_result(data):
    """True when the full video acceptance transcript is present.

    The image is console-capable (package 19): the console is bound to
    the V9958 output, so the boot log has no SIO dynamic-console line;
    the console binding line replaces it."""
    if M8V9958_FAIL in data or M8V9958_UNAVAILABLE in data:
        return False
    return (BANNER in data and
            M8V9958_VIDEO_OK in data and M8V9958_MALFORMED_OK in data and
            M8V9958_RESET_OK in data and M8V9958_CONSOLE_OK in data and
            M8V9958_FILL_OK in data and M8V9958_PATTERN_OK in data and
            M8V9958_SELFTEST_OK in data)


def _m8console_result(data):
    """True when the console acceptance boot transcript is present."""
    if M8CONSOLE_FAIL in data or b"PANIC" in data:
        return False
    return (BANNER in data and M8CONSOLE_INPUT_OK in data and
            M8CONSOLE_OUTPUT_OK in data and M8CONSOLE_SELFTEST_OK in data)


def _m9fs_result(data):
    """True when the M9 filesystem acceptance programs reported success:
    the per-case suite passed, the launcher cleanup check passed, and the
    target hook reported OK, with no FAIL/PANIC marker."""
    if M9FS_FAIL in data or M9FS_SELFTEST_FAIL in data or b"PANIC" in data:
        return False
    return (BANNER in data and M9FS_PASS in data and
            M9FS_LAUNCHER_OK in data and M9FS_SELFTEST_OK in data)


M9EXEC_SELFTEST_OK = b"m9 exec selftest: OK\r\n"
M9EXEC_SELFTEST_FAIL = b"m9 exec selftest: FAIL"

# Package 30: the IOC-FAT image runs the same native suite (its IOC-FAT
# section replaces the ramfs cases) plus the target hook's provider cases.
# The image compiles HID and V9958 out, so its console is SIO/SIO: the
# monitor banner and prompt arrive on the same link, which is what proves
# the ordinary launcher still ran after the acceptance phase.
M9IOCFAT_PASS = b"fstest: PASS "
M9IOCFAT_MOUNTED = b"m9 iocfat: fstest iocfat section passed\r\n"
M9IOCFAT_SELFTEST_OK = b"m9 iocfat selftest: OK\r\n"
M9IOCFAT_SELFTEST_FAIL = b"m9 iocfat selftest: FAIL"
M9IOCFAT_MONITOR = b"Z80> "

# Package 31: the M9 integration image.  Production boot path with `/` on
# the card, and the whole native acceptance: fstest.zex (which exec's the
# deep-matrix program fsfat.zex and prints one transcript), the fsdeep.zex
# depth probe and the target hook's stale/write-fault windows.
M9INTEG_FSTEST_PASS = b"fstest: PASS "
M9INTEG_FSFAT_PASS = b"fsfat: PASS "
M9INTEG_SELFTEST_OK = b"m9 selftest: OK\r\n"
M9INTEG_SELFTEST_FAIL = b"m9 selftest: FAIL"
M9INTEG_ABSENT = b"m9 selftest: OK (card absent)"
M9INTEG_EROFS = b"m9 selftest: OK (read-only card)"
M9INTEG_MONITOR = b"Z80> "


def _m9exec_result(data):
    """True when the M9 SYS_EXEC acceptance reported success: the native
    fstest suite (including its exec cases) passed and the target hook
    reported OK, with no FAIL/PANIC marker."""
    if M9FS_FAIL in data or M9EXEC_SELFTEST_FAIL in data or b"PANIC" in data:
        return False
    return (BANNER in data and M9FS_PASS in data and
            M9EXEC_SELFTEST_OK in data)


def _m9iocfat_result(data):
    """True when the M9 IOC-FAT acceptance image reported success on a
    writable card: `/` is served by iocfat, the native suite's IOC-FAT
    section passed, the target hook reported OK, and the unchanged launcher
    then started the monitor on the same SIO console - with no FAIL/PANIC
    marker and no `/` diagnostic."""
    if (M9FS_FAIL in data or M9IOCFAT_SELFTEST_FAIL in data or
            b"PANIC" in data):
        return False
    return (BANNER in data and M9IOCFAT_PASS in data and
            M9IOCFAT_MOUNTED in data and M9IOCFAT_SELFTEST_OK in data and
            M9IOCFAT_MONITOR in data and b"vfs: / unavailable" not in data)


def _m9integ_result(data):
    """True when the M9 integration image reported success on a writable
    card: the whole native acceptance (fstest.zex and its deep-matrix child
    fsfat.zex) passed, the target hook reported OK, and the unchanged
    launcher then started the monitor on the same SIO console - with no
    FAIL/PANIC marker and no `/` diagnostic."""
    if (M9FS_FAIL in data or M9INTEG_SELFTEST_FAIL in data or
            b"PANIC" in data):
        return False
    return (BANNER in data and M9INTEG_FSTEST_PASS in data and
            M9INTEG_FSFAT_PASS in data and M9INTEG_SELFTEST_OK in data and
            M9INTEG_MONITOR in data and b"vfs: / unavailable" not in data)


def main(argv):
    parser = argparse.ArgumentParser(description="ZephyrOS serial smoke check")
    parser.add_argument("--port", default=os.environ.get("ZOS_SERIAL_PORT", ""))
    parser.add_argument(
        "--baud", type=int, default=int(os.environ.get("ZOS_SERIAL_BAUD", "115200"))
    )
    parser.add_argument("--mode", choices=("normal", "subsystem",
                                           "romfs-corrupt", "m8-ioc",
                                           "m8-hid", "m8-v9958",
                                           "m8-console", "m8",
                                           "m8-fb-nohid",
                                           "m8-fb-nohid-nov9958",
                                           "m8-fb-badioc", "m8-fb-arena",
                                           "m9-fs", "m9-exec",
                                           "m9-iocfat", "m9"),
                        default="normal")
    parser.add_argument(
        "--timeout", type=float, default=float(os.environ.get("ZOS_SERIAL_TIMEOUT", "15"))
    )
    parser.add_argument(
        "--quiet", type=float,
        default=float(os.environ.get("ZOS_SERIAL_QUIET", "5")),
        help="normal mode: quiet window after the banner before a "
             "banner-only PASS (default 5s)")
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
    if args.mode == "m9-fs":
        print("serial_smoke: mode m9-fs checks the filesystem acceptance "
              "boot log; power-cycle the board or press RESET now so the "
              "boot and the two native programs are captured")
    if args.mode == "subsystem":
        print("serial_smoke: mode subsystem checks the boot log and the "
              "interactive monitor step; power-cycle the board or press "
              "RESET now so the boot is captured; the tool sends Q once "
              "the Z80> prompt appears and finishes on the monitor exit "
              "line")
    if args.mode == "romfs-corrupt":
        print("serial_smoke: mode romfs-corrupt checks the boot log for "
              "the corrupt-store panic; power-cycle the board or press "
              "RESET now so the boot is captured")
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
    if args.mode == "m8-v9958":
        print("serial_smoke: mode m8-v9958 checks the boot log and the "
              "video acceptance pass; power-cycle the board or press "
              "RESET now so the boot (including the V9958 selftest) is "
              "captured; the image waits for the host sync sequence")
        print("serial_smoke: visually confirm on the V9958 output: a "
              "white 64x64 rectangle near the upper left, a white-then-"
              "magenta band below it, and the default blue background "
              "(see docs/validation/2026-10-03-m8-v9958.md)")
    if args.mode == "m8-console":
        print("serial_smoke: mode m8-console checks the console "
              "acceptance boot log; power-cycle the board or press "
              "RESET now so the boot (including the console selftest) is "
              "captured; the image waits for the host sync sequence")
        print("serial_smoke: the interactive session runs over the HID "
              "input and the V9958 output; confirm on the physical "
              "keyboard/display that the monitor banner and prompt are "
              "visible and that the documented command pass works "
              "(see docs/validation/2026-10-03-m8-console.md)")
    if args.mode == "m8":
        print("serial_smoke: mode m8 checks the integration image's "
              "serial portion (binding report, PING level, rejected-call "
              "invariants); power-cycle the board or press RESET now so "
              "the boot is captured; the image waits for the host sync "
              "sequence")
        print("serial_smoke: the interactive monitor session runs over "
              "the HID input and the V9958 output; confirm the banner, "
              "prompt and documented command pass on the physical "
              "keyboard/display")
    if args.mode in M8FB_MODES:
        spec = M8FB_MODES[args.mode]
        print("serial_smoke: mode %s checks the fallback boot log "
              "(expected %s input / %s output); power-cycle the board or "
              "press RESET now so the boot is captured; the image waits "
              "for the host sync sequence"
              % (args.mode, spec["input"][0].decode(),
                 spec["output"][0].decode()))
        if spec["input"][1] == b"01":
            print("serial_smoke: when 'm8 fallback: send input' appears, "
                  "this tool sends the invited byte over the serial link "
                  "and validates the read-back")
        if spec["serial_marker"]:
            print("serial_smoke: the console output is the SIO fallback; "
                  "the marker is verified in this transcript")
        else:
            print("serial_smoke: the console output is the V9958; "
                  "visually confirm 'm8 fallback: console output' on the "
                  "display")
    deadline = time.monotonic() + args.timeout
    data = b""
    bridge_drops = 0
    host_sync_at = 0.0
    rx_byte_sent = False
    normal_banner_at = 0.0
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
            if (args.mode in ("m8-ioc", "m8-hid", "m8-v9958",
                              "m8-console", "m8", "m8-fb-nohid",
                              "m8-fb-nohid-nov9958", "m8-fb-badioc",
                              "m8-fb-arena") and
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
                elif args.mode == "romfs-corrupt":
                    if _romfs_corrupt_result(data):
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
                elif args.mode == "m8-v9958":
                    if _m8v9958_result(data) or M8V9958_FAIL in data or \
                            M8V9958_UNAVAILABLE in data:
                        break
                elif args.mode == "m8-console":
                    if _m8console_result(data) or M8CONSOLE_FAIL in data:
                        break
                elif args.mode == "m8":
                    if _m8_result(data) or M8_FAIL in data:
                        break
                elif args.mode == "m9-fs":
                    if _m9fs_result(data) or M9FS_FAIL in data or \
                            M9FS_SELFTEST_FAIL in data:
                        break
                elif args.mode == "m9-exec":
                    if _m9exec_result(data) or M9FS_FAIL in data or \
                            M9EXEC_SELFTEST_FAIL in data:
                        break
                elif args.mode == "m9-iocfat":
                    # Keep reading past the selftest line: the monitor
                    # prompt is part of the contract for this image.
                    if _m9iocfat_result(data) or M9FS_FAIL in data or \
                            M9IOCFAT_SELFTEST_FAIL in data:
                        break
                elif args.mode in M8FB_MODES:
                    if M8FB_SEND_INPUT in data and not rx_byte_sent:
                        try:
                            link.write(M8FB_INPUT_BYTE)
                            link.flush()
                        except (serial.SerialException, OSError):
                            pass  # the read notices a dropped link
                        rx_byte_sent = True
                        print("serial_smoke: sent invited input byte %r"
                              % M8FB_INPUT_BYTE)
                    if _m8fb_result(args.mode, data) or M8FB_FAIL in data:
                        break
            if args.mode == "normal":
                # Banner-only contract: the V9958-bound production console
                # sends only the early banner on serial.  PASS once the
                # banner is complete and the quiet window elapsed with no
                # failure marker; an SIO-fallback bench passes early on the
                # stronger console/monitor evidence.
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
                    "'<module>: unavailable' line; or, on an SIO-fallback "
                    "bench, 'console: dynamic sio0.b OK', the monitor "
                    "banner and the 'Z80> ' prompt" % args.quiet)
        if ok and not _production_strong(data):
            print("serial_smoke: banner-only PASS - the production console "
                  "is bound to the V9958, so serial carries only the banner")
            print("serial_smoke: operator step: confirm on the V9958 the "
                  "monitor banner, the status line and the 'Z80>' prompt, "
                  "then type a key")
    elif args.mode == "subsystem":
        ok = _subsys_complete(data)
        expected = ("the banner, the dynamic console line, the five "
                    "mechanism groups (syscall/irq/romfs/process/loader "
                    "selftest: OK) with no FAIL/PANIC marker, and one "
                    "interactive monitor step ending on 'monitor: exit "
                    "status 0x'")
    elif args.mode == "romfs-corrupt":
        ok = _romfs_corrupt_result(data)
        expected = ("the banner and the early-console marker "
                    "'PANIC: romfs: corrupt recovery image' with no "
                    "module execution")
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
    elif args.mode == "m8-v9958":
        ok = _m8v9958_result(data)
        expected = ("the banner, the v9958.0 registration, the "
                    "malformed-request rejection, the RESET pass, the "
                    "console-binding line, the presented fill, the "
                    "drawn pattern, and 'm8 v9958 selftest: OK'; the "
                    "operator must also confirm the documented pattern "
                    "visually")
    elif args.mode == "m8-console":
        ok = _m8console_result(data)
        expected = ("the banner, the HID input class report, the V9958 "
                    "output class report, and 'm8 console selftest: OK'; "
                    "the operator must also confirm the visible monitor "
                    "session on the V9958 display with the USB keyboard")
    elif args.mode == "m8":
        ok = _m8_result(data)
        expected = ("the banner, the binding report (hid.keyboard0 input "
                    "class 04, v9958.0 output class 02), the device "
                    "count, the PING controller level, the rejected-call "
                    "invariants and 'm8 selftest: OK', with no optional-"
                    "module diagnostic; the operator must also confirm "
                    "the visible monitor session on the V9958 display "
                    "with the USB keyboard")
    elif args.mode == "m9":
        ok = _m9integ_result(data)
        expected = ("the banner, 'fstest: PASS n/n' from the native suite, "
                    "'fsfat: PASS n/n' from the deep-matrix program it exec's "
                    "(namespace, basic, sizes, large, cache, pool, exdev, "
                    "errors), the target hook's fault windows and "
                    "'m9 selftest: OK', and then the monitor prompt "
                    "'Z80> ' on the same SIO console — with a writable FAT "
                    "card and no `vfs: / unavailable` diagnostic")
    elif args.mode == "m9-iocfat":
        ok = _m9iocfat_result(data)
        expected = ("the banner, 'fstest: PASS n/n' from the native suite's "
                    "IOC-FAT section (namespace, romfs, iocfat, window), the "
                    "cache phase markers, the target hook's 'fstest iocfat "
                    "section passed', 'm9 iocfat selftest: OK', and then the "
                    "monitor prompt 'Z80> ' on the same SIO console — with a "
                    "writable FAT card and no `vfs: / unavailable` "
                    "diagnostic")
    elif args.mode == "m9-fs":
        ok = _m9fs_result(data)
        expected = ("the banner, 'fstest: PASS n/n' from the native suite, "
                    "'fstest: launcher OK' from the second-exec cleanup "
                    "check, and 'm9 fs selftest: OK' with no FAIL/PANIC "
                    "marker")
    elif args.mode in M8FB_MODES:
        ok = _m8fb_result(args.mode, data)
        spec = M8FB_MODES[args.mode]
        expected = ("the %s/%s binding report, its exact early-SIO "
                    "diagnostic(s), the scripted serial input byte where "
                    "the input is SIO, the console marker where it is "
                    "serial-visible (otherwise confirmed on the display), "
                    "and 'm8 fallback selftest: OK'"
                    % (spec["input"][0].decode(),
                       spec["output"][0].decode()))

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
    if mode not in ("normal", "m9-iocfat", "m9"):
        return
    if VFS_UNAVAILABLE in data:
        index = data.index(VFS_UNAVAILABLE) + len(VFS_UNAVAILABLE)
        code = data[index:index + 2].decode("ascii", "replace")
        print("serial_smoke: note: the optional fs2 root `/` is NOT mounted "
              "(`vfs: / unavailable 0x%s`); the card is absent, "
              "write-protected, unreadable, or the MCU did not answer the "
              "FS2 root probe" % code)
    elif M9IOCFAT_ABSENT in data or M9INTEG_ABSENT in data:
        print("serial_smoke: note: the optional fs2 root `/` is NOT mounted "
              "(this image reports the absent card as an expected outcome)")
    elif BANNER in data:
        print("serial_smoke: note: the optional fs2 root `/` mounted "
              "without a diagnostic, so the MCU answered the FS2 root probe "
              "with media present")


def _explain_wrong_image(mode, data):
    """Turn an image/mode mismatch into an actionable hint.

    The subsystem and corrupt-store images are flashed separately; a run
    with the wrong --mode otherwise fails with an unexplained timeout on
    the wrong transcript.
    """
    if mode == "subsystem":
        if ROMFS_CORRUPT_PANIC in data:
            print("serial_smoke: hint: the corrupt-store panic was "
                  "observed; the romfs-corrupt image is probably flashed "
                  "(use --mode romfs-corrupt)", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  The "
                  "boot lines (banner and group markers) print once at "
                  "boot; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        elif not SUBSYS_LOADER_OK in data:
            print("serial_smoke: hint: the captured boot log is "
                  "incomplete (one or more mechanism groups did not "
                  "report OK); the capture may have started mid-boot or "
                  "the wrong image is flashed", file=sys.stderr)
        else:
            print("serial_smoke: hint: the groups completed but the "
                  "interactive monitor step did not finish; the "
                  "transcript above shows how far it got (the tool sends "
                  "Q at the Z80> prompt)", file=sys.stderr)
    elif mode == "romfs-corrupt":
        if SUBSYS_LOADER_OK in data or SUBSYS_ROMFS_OK in data:
            print("serial_smoke: hint: the ROMFS success markers were "
                  "observed; the subsystem image is probably flashed "
                  "(use --mode subsystem)", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured; the "
                  "panic marker prints once at boot.  Start this tool "
                  "first, then power-cycle the board or press RESET "
                  "while it waits, and rerun", file=sys.stderr)
    elif mode == "normal":
        if b"PANIC" in data or any(marker in data
                                   for marker in PRODUCTION_CONSOLE_FAIL):
            print("serial_smoke: hint: the production boot reported a "
                  "panic or a console failure (see the marker in the "
                  "transcript above)", file=sys.stderr)
        elif BANNER in data and CONSOLE_DYNAMIC_OK not in data:
            print("serial_smoke: hint: only the early serial banner was "
                  "captured.  The production image binds its console to "
                  "the V9958 output (architecture section 30.9), so "
                  "'console: dynamic sio0.b OK', the monitor banner and "
                  "the 'Z80> ' prompt are rendered on the V9958 display "
                  "and never reach the SIO port; --mode normal can only "
                  "gate an SIO-fallback bench.  Confirm the monitor "
                  "prompt and HID input on the V9958 display; that is "
                  "the accepted production gate until the mode is split "
                  "(see docs/validation/"
                  "2026-10-05-m9-provider-abi-hardware.md section 5)",
                  file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
    elif mode == "m9":
        if M9INTEG_SELFTEST_FAIL in data:
            print("serial_smoke: hint: the integration acceptance failed "
                  "while `/` was mounted; record the FAIL code, the first "
                  "failing case line above, and the card state in "
                  "docs/validation/2026-10-05-m9-filesystem.md",
                  file=sys.stderr)
        elif VFS_UNAVAILABLE in data or M9INTEG_ABSENT in data:
            print("serial_smoke: hint: `/` is not mounted, so this image "
                  "took its absent-card path; the integration row needs a "
                  "writable FAT card inserted", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
    elif mode == "m9-iocfat":
        if M9IOCFAT_SELFTEST_FAIL in data:
            print("serial_smoke: hint: the provider selftest failed while "
                  "`/` was mounted.  If the card is not inserted, the MCU "
                  "still answered the FS2 root probe (a phantom volume), so "
                  "the first real read or write is where it failed - record "
                  "the FAIL code from the transcript above and the card "
                  "state in docs/validation/"
                  "2026-10-05-m9-iocfat-provider.md section 7",
                  file=sys.stderr)
        elif VFS_UNAVAILABLE in data:
            print("serial_smoke: hint: `/` is not mounted, so this image "
                  "took its absent-card path; row 3 needs a writable FAT "
                  "card inserted", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  Start "
                  "this tool first, then power-cycle the board or press "
                  "RESET while it waits, and rerun", file=sys.stderr)
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
        elif SUBSYS_LOADER_OK in data:
            print("serial_smoke: hint: the subsystem image markers were "
                  "observed; the subsystem image is probably flashed "
                  "instead of build-m8-ioc-selftest/"
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
    elif mode == "m8-v9958":
        if M8V9958_UNAVAILABLE in data or M8V9958_FAIL in data:
            print("serial_smoke: hint: the V9958 selftest did not "
                  "complete (the module is unavailable or the "
                  "operation/pattern phase failed).  Check the LCD/CRT "
                  "output, and that "
                  "build-m8-v9958-selftest/"
                  "zephyros-m8-v9958-selftest.bin is flashed",
                  file=sys.stderr)
        elif M8HID_SELFTEST_OK in data:
            print("serial_smoke: hint: the M8 HID marker was observed; "
                  "the HID image is probably flashed instead of "
                  "build-m8-v9958-selftest/"
                  "zephyros-m8-v9958-selftest.bin", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        else:
            print("serial_smoke: hint: the transcript is incomplete; the "
                  "last marker above shows how far the video pass got.  "
                  "Also confirm the documented pattern visually on the "
                  "V9958 output", file=sys.stderr)
    elif mode == "m8-console":
        if M8CONSOLE_FAIL in data:
            print("serial_smoke: hint: the console selftest reported a "
                  "failure (the detail code on the FAIL line names the "
                  "phase).  Check the V9958 output and the USB "
                  "keyboard, and that "
                  "build-m8-console-selftest/"
                  "zephyros-m8-console-selftest.bin is flashed",
                  file=sys.stderr)
        elif M8V9958_SELFTEST_OK in data:
            print("serial_smoke: hint: the V9958 image marker was "
                  "observed; the video image is probably flashed "
                  "instead of build-m8-console-selftest/"
                  "zephyros-m8-console-selftest.bin", file=sys.stderr)
        elif not BANNER in data:
            print("serial_smoke: hint: no boot log was captured.  This "
                  "image waits for the host sync sequence before the "
                  "banner; start this tool first, then power-cycle the "
                  "board or press RESET while it waits, and rerun",
                  file=sys.stderr)
        else:
            print("serial_smoke: hint: the transcript is incomplete; "
                  "the last marker above shows how far the boot got.  "
                  "The interactive session runs over the HID input "
                  "and the V9958 output and must be confirmed on the "
                  "physical keyboard and display", file=sys.stderr)
    elif mode == "m8":
        if M8IOC_SELFTEST_OK in data or M8CONSOLE_SELFTEST_OK in data:
            print("serial_smoke: hint: an earlier M8 image marker was "
                  "observed; flash build-m8-selftest/"
                  "zephyros-m8-selftest.bin (the integration image)",
                  file=sys.stderr)
        elif M8_UNAVAILABLE_ANY in data:
            print("serial_smoke: hint: an optional module reported "
                  "unavailable; the integration image carries all four "
                  "modules, so the recovery store or the flashed image "
                  "is wrong.  The exact code above names the failed load",
                  file=sys.stderr)
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
    elif mode in M8FB_MODES:
        spec = M8FB_MODES[mode]
        if b"m8 selftest: OK" in data:
            print("serial_smoke: hint: the integration image appears to "
                  "be flashed; use --mode m8", file=sys.stderr)
        print("serial_smoke: hint: this mode expects the %s/%s binding "
              "and the exact diagnostic '%s'; the transcript above shows "
              "what the flashed image actually provides"
              % (spec["input"][0].decode(), spec["output"][0].decode(),
                 spec["diagnostics"][0].decode().rstrip()),
              file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
