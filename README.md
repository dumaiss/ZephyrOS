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
- **M2 — Interrupt executive**: kernel-owned IM2 (`I=FDh`), fixed-source
  one-owner registry, mode-safe full-context IRQ entry/return with
  dedicated common/bank-7 stacks, `FFh` guard identity, CTC0 acceptance
  image, and linked-byte simulation.  Accepted on physical hardware
  2026-10-02 (`irq selftest: OK`).
- **M3 — Device registry and static drivers**: fixed 16-entry bank-7
  device registry with capability lookup and registration
  checkpoint/rollback, versioned driver ABI with an explicit static driver
  table, polling SIO0/B driver (`sio0.b`), capability-bound console
  service, and a test-only bank-7 IRQ probe claiming CTC0 through the M2
  executive (`docs/devices.md`).  Accepted on physical hardware
  2026-10-02 (`console: sio0.b OK`, `m3 selftest: OK`, console RX round
  trip; see `docs/validation/2026-10-02-m3-acceptance.md`).
- Stage `04-m1-validation-acceptance` is accepted.

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
make irq-selftest
                   # target-side M2 IRQ executive image (build-irq-selftest/)
make m3-selftest
                   # M3 acceptance image: banner + console service line +
                   # syscall/IRQ/M3 self-tests (build-m3-selftest/)
make m3-fallback
                   # disabled-drivers / disabled-binding fallback images
                   # (build-m3-nodrv/, build-m3-nobind/)
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the banner from physical hardware
make smoke-hardware-selftest SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify 'syscall selftest: OK'
make smoke-hardware-irq SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify 'irq selftest: OK'
make smoke-hardware-m3 SERIAL_PORT=/dev/ttyUSB0
                   # capture the M3 transcript; sends 'R' for the console
                   # RX round trip when the kernel announces readiness
make clean         # remove generated files under build*/
```

Every build automatically runs `tools/mapcheck.py` (authoritative
post-link layout check) and `tools/pack_image.py` (byte-level image
validation, page packing, `build/zephyros.manifest.json` with sizes,
hashes, fill counts, and entry addresses).  `make check` additionally
proves that a clean rebuild reproduces the identical binary.

Physical-hardware validation uses `tools/serial_smoke.py` (115200 8N1,
bounded timeout).  Dated acceptance records live under `docs/validation/`
(M1, M2, and the M3 record).

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
checklist.  Expected output (M3 adds the console-service line):

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
console: sio0.b OK
```

## Layout

```text
ZephyrOS/
├── Makefile
├── docs/                  architecture.md, boot.md, memory.md, abi.md,
│                          devices.md
├── include/zephyros/      types.h, platform.h, kernel.h, syscall.h,
│                          errno.h, selftest.h, irq.h, device.h,
│                          driver.h, console.h, m3_selftest.h
├── boot/start.asm         common boot entry + C runtime initialization
├── kernel/                main.c, panic.c, early_console.c, syscall.c,
│                          device/ (registry.c, driver.c, console.c),
│                          irq/
├── drivers/               sio/ (SIO0/B static driver)
├── platform/zephyr80/     hardware.h/.inc, memory.inc, reset.asm,
│                          banking.asm, page_zero.asm, syscall_entry.asm,
│                          sdcc_helpers.asm, selftest.asm/.inc
├── linker/zephyr80.lk     sdldz80 control (canonical build/)
├── tools/                 pack_image.py, mapcheck.py, serial_smoke.py
└── tests/                 host-side image/layout/ABI tests + Z80 replays
```
