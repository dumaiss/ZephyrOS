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

The device is given with --port or the ZOS_SERIAL_PORT environment
variable (the SERIAL_PORT make variable is passed through).  A missing or
unopenable device fails clearly; ordinary host-only `make test` never
invokes this tool.

Usage:
    serial_smoke.py --port /dev/ttyUSB0 [--mode normal|selftest|irq|m3|m4|m4-failure|m5|m5-corrupt]
                   [--baud 115200] [--timeout 15]

Exit status: 0 success, 1 output mismatch/incomplete, 2 device unavailable.
"""

import argparse
import os
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


def main(argv):
    parser = argparse.ArgumentParser(description="ZephyrOS serial smoke check")
    parser.add_argument("--port", default=os.environ.get("ZOS_SERIAL_PORT", ""))
    parser.add_argument(
        "--baud", type=int, default=int(os.environ.get("ZOS_SERIAL_BAUD", "115200"))
    )
    parser.add_argument("--mode", choices=("normal", "selftest", "irq", "m3",
                                           "m4", "m4-failure", "m5",
                                           "m5-corrupt"),
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

    try:
        link = serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.2,
        )
    except serial.SerialException as exc:
        print("serial_smoke: cannot open %s: %s" % (args.port, exc), file=sys.stderr)
        return 2

    print("serial_smoke: listening on %s at %d 8N1 (mode=%s, timeout=%gs)"
          % (args.port, args.baud, args.mode, args.timeout))
    deadline = time.monotonic() + args.timeout
    data = b""
    rx_byte_sent = False
    try:
        while time.monotonic() < deadline:
            chunk = link.read(256)
            if chunk:
                data += chunk
                if args.mode == "normal" and BANNER in data:
                    break
                if args.mode == "selftest" and BANNER in data and SELFTEST_OK in data:
                    break
                if args.mode == "irq" and BANNER in data and IRQ_SELFTEST_OK in data:
                    break
                if args.mode == "m3":
                    if M3_RX_PROMPT in data and not rx_byte_sent:
                        link.write(M3_RX_BYTE)
                        link.flush()
                        rx_byte_sent = True
                        print("serial_smoke: sent RX byte %r" % M3_RX_BYTE)
                    if (CONSOLE_DYNAMIC_OK in data and
                            M3_SELFTEST_OK in data):
                        break
                if args.mode == "m4":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M4_SELFTEST_OK in data and
                            M4_SIO_PREFIX in data and
                            M4_ACCEPT_OK in data):
                        break
                if args.mode == "m4-failure":
                    if (BANNER in data and CONSOLE_DYNAMIC_FAIL in data and
                            M4_FAILURE_OK in data):
                        break
                if args.mode == "m5":
                    if (BANNER in data and CONSOLE_DYNAMIC_OK in data and
                            M5_SELFTEST_OK in data):
                        break
                if args.mode == "m5-corrupt":
                    if BANNER in data and M5_CORRUPT_PANIC in data:
                        break
    finally:
        link.close()

    print("serial_smoke: captured %d bytes" % len(data))
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


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
