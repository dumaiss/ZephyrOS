# ZephyrOS

ZephyrOS is the native operating system for the Zephyr-80.  The early
milestones establish a clean boot path, a stable kernel execution
environment, and the architectural boundaries later subsystems will build
on.  See `docs/architecture.md` for the contract and `docs/boot.md` for the
M0 boot mechanics.

## Status

- **M0 — Kernel bring-up**: boot from the 128 KiB ROM image, copy into
  SRAM, enter the kernel in memory mode `11`, initialize the polled early
  serial console, print the banner, and halt.  Accepted on physical
  hardware 2026-10-02 (exact banner observed; see `docs/boot.md`).
- **M1 — Memory/banking foundation**: explicit mode `10`/`11` region
  ownership (`docs/memory.md`), common-resident banking primitives and
  mode gates, page-zero and transition-state reservation, boot-time latch
  assertion.  Accepted on physical hardware 2026-10-02 (banner unchanged).
- **M1 — Native syscall scaffold**: `RST 30h` vector in native page zero,
  common-memory entry/exit gate, bank-7 dispatcher returning `ZOS_ENOSYS`
  (`docs/abi.md`), target-side smoke self-test (`make syscall-selftest`).
  Accepted on physical hardware 2026-10-02 (`syscall selftest: OK`).
- Stage `04-m1-validation-acceptance` is pending.

## Prerequisites

- SDCC 4.x (`sdcc`, `sdasz80`, `sdldz80`) with the Z80 target
- GNU Make
- Python 3 (host image tooling and tests)

The kernel is freestanding: no hosted C runtime or standard library is
linked.

## Build

```sh
make               # build build/zephyros.bin and build/zephyr80.bin
make test          # host image/layout/ABI tests + artifact integrity checks
make check         # reproducible-rebuild proof + full validation stack
make syscall-selftest
                   # target-side RST 30h round-trip image (build-selftest/)
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the banner from physical hardware
make smoke-hardware-selftest SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify 'syscall selftest: OK'
make clean         # remove generated files under build/ and build-selftest/
```

Every build automatically runs `tools/mapcheck.py` (authoritative
post-link layout check) and `tools/pack_image.py` (byte-level image
validation, page packing, `build/zephyros.manifest.json` with sizes,
hashes, fill counts, and entry addresses).  `make check` additionally
proves that a clean rebuild reproduces the identical binary.

Physical-hardware validation uses `tools/serial_smoke.py` (115200 8N1,
bounded timeout).  The dated acceptance record is
`docs/validation/2026-10-02-m1-acceptance.md`.

Artifacts (all under `build/`):

```text
zephyros.ihx   linker output
zephyros.map   symbol/area map
page0.bin      ROM page 0 (64 KiB, copied to SRAM bank 0)
page1.bin      ROM page 1 (64 KiB, copied to SRAM bank 7)
zephyros.bin   128 KiB boot image (FF-filled)
zephyr80.bin   identical copy for MAME
```

The image is exactly 131072 bytes: ROM page 0 followed by ROM page 1.
`tools/pack_image.py` turns memory-layout violations into build errors
(out-of-region content, copier mirroring, entry-point placement), and
`tools/mapcheck.py` validates area ownership, common-resident gate
placement, and the stack reservation.  The implemented memory model is
documented in `docs/memory.md`.

## Run in the emulator

See `docs/boot.md` for the MAME invocation and the physical-ROM test
checklist.  Expected output:

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
```

## Layout

```text
ZephyrOS/
├── Makefile
├── docs/                  architecture.md, boot.md, memory.md, abi.md
├── include/zephyros/      types.h, platform.h, kernel.h, syscall.h,
│                          errno.h, selftest.h
├── boot/start.asm         common boot entry + C runtime initialization
├── kernel/                main.c, panic.c, early_console.c, syscall.c
├── platform/zephyr80/     hardware.h/.inc, memory.inc, reset.asm,
│                          banking.asm, page_zero.asm, syscall_entry.asm,
│                          selftest.asm/.inc
├── linker/zephyr80.lk     sdldz80 control
├── tools/                 pack_image.py, mapcheck.py
└── tests/                 host-side image/layout/ABI tests
```