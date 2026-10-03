# M0/M1 Acceptance Record — 2026-10-02

Status: **M0 accepted (hardware observed). M1 memory/banking and syscall
scaffold accepted (hardware observed).**  Emulator validation is not
available in this environment (the local Zephyr-80 MAME driver is not
configured for the mode `10`/`11` banking decode), so target evidence is
physical-hardware serial output only.

## Toolchain

| Tool | Version |
| --- | --- |
| SDCC | 4.5.0 #15242 (Linux) |
| sdasz80 / sdldz80 | SDCC 4.5.0 package (asxxxx V03.00/V05.40) |
| GNU Make | 4.4.1 |
| Python | 3.14.4 |

## Commands

```sh
make clean
make                    # normal image + auto mapcheck + manifest
make test               # 68 host tests
make check              # reproducible-rebuild proof + full validation stack
make syscall-selftest   # self-test image + mapcheck
```

## Host-check results (automated, completed)

- `make test`: 68/68 tests pass — image/layout, constant parity, latch
  formulas, mapcheck units, negative CLI fixtures for every rejection
  class, banner regression, and the Z80-simulated `RST 30h` round trip for
  banks 0 and 6.
- `make check`: clean rebuild reproduces the identical SHA-256; mapcheck,
  packer, and artifact integrity checks all pass.
- mapcheck: normal build OK (8 areas, 66 symbols), selftest build OK
  (8 areas, 75 symbols).
- Compiler warnings are errors (`--Werror`).

## Binary hashes

| Artifact | SHA-256 |
| --- | --- |
| `build/zephyros.bin` (normal) | `42d5b314cd05062749d7a83812736e513dfcfd4fdef7c51b0b5eb8bc7a62aa31` |
| `build-selftest/zephyros-selftest.bin` | `771c9ffe911ab917e4590a6486eb32e1d5cb4fcaa651609fc47e228d89eab82e` |

The normal image is exactly 131072 bytes; `build/zephyr80.bin` is
byte-identical (`cmp`).

## Serial settings

- Device: SIO0/B (data `22h`, control `23h`), 115200 8N1, raw, no flow
  control, polled.
- Capture: `tools/serial_smoke.py --port /dev/ttyUSB0 --mode normal|selftest`
  (or `make smoke-hardware SERIAL_PORT=/dev/ttyUSB0`); bounded timeout,
  clear failure when the device is absent or output is incomplete.

## Physical-hardware results (observed serial output, human-confirmed)

| Test | Result |
| --- | --- |
| M0 normal image | **PASS** — exact banner observed: `ZephyrOS 0.0` / `platform: Zephyr-80` / `kernel entry: OK` / `serial: OK` / `HALT`, then halt loop |
| M1 banking re-flash | **PASS** — same banner, no boot regression |
| M1 syscall selftest (first build) | FAIL `bank=0 code=0xFF` — root-caused: gate clobbered caller `BC` before saving it; fail codes never stored. Fixed. |
| M1 syscall selftest (fixed build) | **PASS** — `syscall selftest: OK` (round trip in application banks 0 and 6) |

## Emulator

**Not available.**  The local Zephyr-80 MAME driver does not implement the
architecture's mode `10`/`11` banking, so no emulator evidence is claimed;
host checks plus physical-hardware observations are the recorded evidence.

## Known M1 limitations

- Single-entry, non-reentrant syscall gate; interrupts disabled; alternate
  registers not part of the ABI.
- Native syscall argument blocks must lie in `0000h-1FFFh` until staging
  APIs exist.
- M1 returns `ZOS_ENOSYS` for every syscall number.
