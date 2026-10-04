# ZephyrOS

ZephyrOS is the native operating system for the Zephyr-80.  The early
milestones establish a clean boot path, a stable kernel execution
environment, and the architectural boundaries later subsystems will build
on.  See `docs/architecture.md` for the contract and `docs/boot.md` for the
reset/boot path (it separates the historical M0 banner transcript from the
current M8 boot).

## Status

- **M0 — Kernel bring-up**: boot from the M0-era 128 KiB ROM image (the
  current image is 512 KiB; see the artifact list below), copy into SRAM,
  enter the kernel in memory mode `11`, initialize the polled early serial
  console, and print the banner the M0 image then halted after.  Accepted
  on physical hardware 2026-10-02 (exact banner observed; see
  `docs/boot.md`).
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
  trip; see `docs/validation/2026-10-02-m3-acceptance.md`).  M4 builds of
  the same image print `console: dynamic sio0.b OK` because the driver is
  then loaded from the module catalog.
- **M4 — Module loader (stage 07)**: linker-reserved `A000h-BFFFh`
  bank-7 driver arena (16 x 512-byte granules), packed ZMOD v1 driver
  containers, host `tools/zmodpack.py` (pack/validate/dump), a
  transactional first-fit kernel loader with relocation, BSS, device/IRQ
  claim rollback, and `module_code_allowed` handler validation, plus
  synthetic fixture modules proven by `make zmod-test` and
  `make m4-loader-selftest`.  Hardware-validated on the physical
  Zephyr-80 2026-10-02 (`m4 loader selftest: OK`; see
  `docs/validation/2026-10-02-m4-loader-hardware.md`).
- **M4 — Dynamic driver acceptance (stage 08)**: the SIO driver is no
  longer linked into the kernel.  It is packed as a ZMOD module
  (`build/modules/sio.zmod`, plus IRQ self-test and init-failure
  variants), embedded in every image's boot catalog, loaded through the
  generic loader at boot, and bound by the console service
  (`console: dynamic sio0.b OK`).  A placement image at a shifted arena
  base (9E00h-BE00h) embeds the identical module bytes, and a failure
  image proves exact rollback.  Hardware-validated on the physical
  Zephyr-80 2026-10-02 after fixing a CTC0 start/claim race
  (`console: dynamic sio0.b OK`, `m4 selftest: sio at 0xA000`; placement
  run `sio at 0x9E00`; failure run `m4 failure selftest: OK`).
  Proven by `make m4-selftest`, `make m4-placement-selftest`, and
  `make m4-failure-selftest`; see `docs/module-format.md` and
  `docs/validation/2026-10-02-m4-acceptance.md`.
- **Stage 10 — production/test layout cleanup**: driver directories now
  contain device implementation only (`drivers/sio/sio_module.c` is the
  production module entry), ZMOD manifests/runtime support live under
  `modules/`, all milestone acceptance code lives under `tests/target/`
  and `tests/modules/` behind the single `ZOS_TARGET_TEST_BUILD` seam,
  and the M3 static-driver path is retired.  Host validation is green
  (`make check` plus every acceptance target); because the kernel layout
  changed, the physical smoke runs must be repeated before citing
  hardware acceptance for the new images (see `docs/validation/`).
- Stage `04-m1-validation-acceptance` is accepted.
- **M5 — ROM recovery filesystem**: safe runtime ROM access through the
  mirrored stackless stub, a validated read-only ROMFS v1 recovery store
  (pages 2-7 of the 512 KiB ROM) with the host packer, and the boot ZMOD
  source moved from the embedded catalog to ROMFS.  Accepted on physical
  hardware 2026-10-03 (success and corrupt-metadata images; see
  `docs/validation/2026-10-03-m5-romfs.md`).
- **M6 — Native process and ZEX v1**: fixed process descriptors, lowest
  free application-bank allocation, a deterministic native page zero,
  the packed single-bank ZEX v1 executable format (`docs/zex-format.md`),
  the native console/exit/getinfo syscalls, and synchronous child
  exec/exit with exact parent restoration.  Accepted on physical
  hardware 2026-10-03 (`docs/validation/2026-10-03-m6-zex.md`).
- **M7 — Native monitor and diagnostic foundation**: `monitor.zex` as an
  ordinary ZEX in ROMFS, the `SYS_DIAG_*` recovery boundary, reusable
  `user/runtime/` program support, the temporary pre-shell launcher, and
  the interactive monitor (`docs/monitor.md`).  Host-validated and
  physically accepted 2026-10-03
  (`docs/validation/2026-10-03-m7-monitor.md`).
- **M8 — Native device stack**: driver ABI v2 with a linker-derived
  production driver arena, the IOC command+bulk transport, the HID
  keyboard/text class over IOC, the V9958 video core with the ported
  85x26 terminal renderer, the preferred HID/V9958 console binding with
  SIO fallback, and the independent direct early/panic SIO path
  (`docs/devices.md` sections 5-8.2).  Packages 14-19 are individually
  accepted; the package-20 integration acceptance (deterministic boot
  order, precise failure diagnostics, the failure/fallback image matrix,
  production-byte simulator replay, build/manifest reconciliation and
  the final physical gate) is recorded in
  `docs/validation/2026-10-03-m8-device-stack.md`.  **Status: complete
  — host/simulator validation and the integrated physical acceptance
  (serial smoke, the full human HID/V9958 monitor pass and the
  fallback-image matrix with its display confirmations) passed
  2026-10-04.**

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
                   # syscall/IRQ/M3 acceptance tests (build-m3-selftest/)
make zmod-test
                   # build/pack/validate the SIO module variants and the
                   # synthetic ZMOD fixtures; host packer tests
                   # (build/modules/)
make m4-loader-selftest
                   # M4 loader acceptance image + simulator replay
                   # (build-m4-loader-selftest/)
make m4-selftest
                   # M4 dynamic-driver acceptance: embedded SIO module
                   # (IRQ self-test variant) + simulator replay
                   # (build-m4-selftest/)
make m4-placement-selftest
                   # identical module bytes at a shifted arena base
                   # (build-m4-placement-selftest/)
make m4-failure-selftest
                   # init-failure module + rollback verification
                   # (build-m4-failure/)
make m5-selftest
                   # M5 ROMFS success image + simulator replay
                   # (build-m5-selftest/)
make m5-corrupt-selftest
                   # M5 corrupt-recovery-image panic image
                   # (build-m5-corrupt-selftest/)
make m6-selftest
                   # M6 ZEX acceptance: two-bank/repeated-cycle/raw-return/
                   # malformed-matrix runs + simulator replay
                   # (build-m6-selftest/)
make m6-failure-selftest
                   # truncated ZEX rollback + valid-copy image
                   # (build-m6-failure-selftest/)
make monitor-test
                   # pack/validate monitor.zex + ABI/layout/dependency tests
make m7-selftest
                   # M7 image: kernel diagnostic selftest + scripted
                   # interactive monitor replay (build-m7-selftest/)
make ioc-test
                   # pack/validate ioc.zmod + host transport contract suite
make hid-test
                   # pack/validate hid.zmod + host class/artifact suite
make v9958-test
                   # pack/validate v9958.zmod + font pipeline + renderer
                   # model suites
make m8-selftest
                   # M8 integration acceptance: full four-module image +
                   # package-20 target diagnostic + monitor launcher, and
                   # the production-byte simulator replay (boot order,
                   # binding, IOC command/bulk, HID queue, scripted
                   # monitor session, post-fault recovery)
                   # (build-m8-selftest/)
make m8-fallback-selftests
                   # builds every failure/fallback variant image (no HID,
                   # no HID+V9958, malformed IOC, arena exhaustion, font
                   # failure, HID rollback) and replays each against its
                   # exact binding and early-SIO diagnostics
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the banner from physical hardware
make smoke-hardware-selftest SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify 'syscall selftest: OK'
make smoke-hardware-irq SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify 'irq selftest: OK'
make smoke-hardware-m3 SERIAL_PORT=/dev/ttyUSB0
                   # capture the M3 transcript; sends 'R' for the console
                   # RX round trip when the kernel announces readiness
make smoke-hardware-m4 SERIAL_PORT=/dev/ttyUSB0
                   # capture the M4 acceptance transcript: banner +
                   # 'console: dynamic sio0.b OK' + 'm4 loader selftest:
                   # OK' + 'm4 selftest: sio at 0x...' + 'm4 selftest: OK'
make smoke-hardware-m4-failure SERIAL_PORT=/dev/ttyUSB0
                   # capture the M4 failure transcript: banner +
                   # 'console: dynamic driver failed' +
                   # 'm4 failure selftest: OK'
make smoke-hardware-m5 SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the M5 ROMFS success transcript
make smoke-hardware-m5-corrupt SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the M5 corrupt-image panic marker
make smoke-hardware-m6 SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the M6 ZEX acceptance transcript
make smoke-hardware-m6-failure SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the M6 rollback transcript
make smoke-hardware-m7 SERIAL_PORT=/dev/ttyUSB0
                   # script + verify the M7 monitor session (help, R,
                   # dumps, modify/readback, I/O, rejections, two Q
                   # cycles); start it first, then power-cycle/RESET the
                   # board so the boot log is captured
make smoke-hardware-m8 SERIAL_PORT=/dev/ttyUSB0
                   # capture + verify the integration image's serial
                   # portion (binding report, PING level, rejected-call
                   # invariants, 'm8 selftest: OK'); then perform the
                   # human HID/V9958 monitor pass on the display
make smoke-hardware-m8-fb-nohid SERIAL_PORT=/dev/ttyUSB0
                   # fallback variants (also -fb-nohid-nov9958,
                   # -fb-badioc, -fb-arena): each verifies its exact
                   # binding, early-SIO diagnostic and scripted serial
                   # input where the input is SIO; V9958-bound output is
                   # confirmed on the display by the operator
make clean         # remove generated files under build*/
```

Every build automatically runs `tools/mapcheck.py` (authoritative
post-link layout check) and `tools/pack_image.py` (byte-level image
validation, page packing, `build/zephyros.manifest.json` with sizes,
hashes, fill counts, and entry addresses).  `make check` additionally
proves that a clean rebuild reproduces the identical binary.

Physical-hardware validation uses `tools/serial_smoke.py` (115200 8N1,
bounded timeout).  The M7 and M8 modes must be listening before the
board boots: start the tool, then power-cycle or press RESET (the
boot-log lines are printed once; the M8 images additionally wait for
the tool's host-sync sequence before the banner).  The `m8` and
`m8-fb-*` modes assert the exact binding and diagnostics of the
integration and fallback images, scripting the serial input where the
bound input is SIO; V9958-bound output is confirmed on the display by
the operator.  Dated acceptance records live under `docs/validation/`
(M1, M2, the M3 record, the M4 loader/acceptance records, M5-M7, and
the package-20 M8 device-stack record).

Artifacts (all under `build/`):

```text
zephyros.ihx   linker output
zephyros.map   symbol/area map
page0.bin ... page7.bin   the eight 64 KiB ROM pages
zephyros.bin   512 KiB boot image (pages 0-7, FF-filled)
zephyr80.bin   identical copy for MAME
recovery.romfs logical ROMFS recovery stream (pages 2-7 payload)
modules/       packed ZMOD modules (+ dump/report JSON); the production
               boot set is sourced from ROMFS, while pre-M5 acceptance
               images embed a catalog blob
```

The image is exactly 524288 bytes: eight 64 KiB pages -- ROM page 0 and
page 1 (the mirrored boot copies) followed by the ROMFS recovery store in
pages 2-7.
`tools/pack_image.py` turns memory-layout violations into build errors
(out-of-region content, copier mirroring, entry-point placement), and
`tools/mapcheck.py` validates area ownership, common-resident gate
placement, and the stack reservation.  The implemented memory model is
documented in `docs/memory.md`.

## Run in the emulator

See `docs/boot.md` for the MAME invocation and the physical-ROM test
checklist.  Expected output (M4 adds the dynamic console-service line):

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
console: dynamic sio0.b OK
```

On the M8 production stack the console-service line is written through
the *bound* console (HID input / V9958 output), so it appears on the
V9958 display rather than the serial link; the serial transcript keeps
the early-SIO banner and any `<name>: unavailable 0x<ZOS_E*>`
diagnostics.  `make smoke-hardware-m8` verifies the serial portion of
the integration image.

## Layout

```text
ZephyrOS/
├── Makefile
├── docs/                  architecture.md, boot.md, memory.md, abi.md,
│                          devices.md, module-format.md,
│                          romfs-format.md, zex-format.md, monitor.md
├── include/zephyros/      types.h, platform.h, kernel.h, syscall.h,
│                          errno.h, irq.h, device.h, driver.h,
│                          console.h, module.h, romfs.h, process.h,
│                          zex.h, ioc.h, hid.h, video.h
├── boot/start.asm         common boot entry + C runtime initialization
├── boot/shadow_copy.asm   reset/ROM-to-SRAM copier
├── kernel/                main.c, panic.c, early_console.c, syscall.c,
│                          memory/ (banking.asm, user_copy.asm),
│                          syscall/ (entry.asm, page_zero.asm, diag.c,
│                          diag_io.asm), runtime/ (sdcc_helpers.asm),
│                          device/ (registry.c, console.c),
│                          irq/, module/ (arena.asm, loader.c,
│                          relocate.c, embedded.c),
│                          romfs/ (romfs.c, rom_access.asm),
│                          process/ (process.c, exec.c, launch.asm)
├── drivers/               sio/ (sio.c, sio_io.asm, sio_module.c),
│                          ioc/, hid/, v9958/ (M8 device modules),
│                          ctc/ (ctc.c)
├── modules/               manifests/ (production ZMOD metadata),
│                          runtime/ (shared module-local SDCC glue)
├── platform/zephyr80/     constants/configuration only: hardware.h/.inc,
│                          memory.h/.inc, irq.h/.inc, config.mk
├── linker/zephyr80.lk     sdldz80 control (canonical build/)
├── user/                  runtime/ (M6 syscall + M7 diagnostic wrappers,
│                          console/hex/dump/status components),
│                          monitor/ (native monitor.zex sources)
├── recovery/manifest.json production ROMFS contents (sio.zmod, ioc.zmod,
│                          hid.zmod, v9958.zmod, v9958-6x8.font,
│                          monitor.zex)
├── tools/                 pack_image.py, mapcheck.py, zmodpack.py,
│                          romfspack.py, zexpack.py, fontpack.py,
│                          serial_smoke.py
├── tests/                 host-side image/layout/ABI tests + Z80 replays;
│                          m5/..m8 (manifests, fixtures, generators);
│                          modules/ (test ZMOD sources + manifests);
│                          target/ (m1..m8 acceptance code, headers,
│                          shared runner)
└── work/                  milestone worker contracts
```
