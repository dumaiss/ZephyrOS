#!/usr/bin/env python3
"""ZephyrOS physical-hardware serial smoke capture.

Opens the SIO0/B serial link (115200 8N1, raw) on the given device,
captures output with a bounded timeout, and verifies the expected output:

    --mode normal    the exact M0 banner, then the halt loop
    --mode selftest  the banner plus "syscall selftest: OK"
    --mode irq       the banner plus "irq selftest: OK"
    --mode m3        the M3 image: banner, "console: sio0.b OK" through the
                     normal console service, all embedded self-tests, and
                     the interactive console RX check (the tool sends 'R'
                     when the kernel announces "ready for RX")

The device is given with --port or the ZOS_SERIAL_PORT environment
variable (the SERIAL_PORT make variable is passed through).  A missing or
unopenable device fails clearly; ordinary host-only `make test` never
invokes this tool.

Usage:
    serial_smoke.py --port /dev/ttyUSB0 [--mode normal|selftest|irq|m3]
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
CONSOLE_OK = b"console: sio0.b OK\r\n"
M3_RX_PROMPT = b"m3 selftest: ready for RX\r\n"
M3_SELFTEST_OK = b"m3 selftest: OK\r\n"
M3_RX_BYTE = b"R"


def main(argv):
    parser = argparse.ArgumentParser(description="ZephyrOS serial smoke check")
    parser.add_argument("--port", default=os.environ.get("ZOS_SERIAL_PORT", ""))
    parser.add_argument(
        "--baud", type=int, default=int(os.environ.get("ZOS_SERIAL_BAUD", "115200"))
    )
    parser.add_argument("--mode", choices=("normal", "selftest", "irq", "m3"),
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
                    if CONSOLE_OK in data and M3_SELFTEST_OK in data:
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
    else:
        ok = (BANNER in data and CONSOLE_OK in data and
              SELFTEST_OK in data and IRQ_SELFTEST_OK in data and
              M3_SELFTEST_OK in data)
        expected = ("the banner, console service line, syscall/irq/m3 "
                    "self-tests, and the console RX round trip")

    if not ok:
        print("serial_smoke: FAIL - %s was not observed (timeout %gs)"
              % (expected, args.timeout), file=sys.stderr)
        return 1
    print("serial_smoke: OK (%s)" % expected)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
