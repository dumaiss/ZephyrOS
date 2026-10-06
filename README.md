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
  (`docs/abi.md`), target-side smoke self-test.
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
  trip; see `docs/validation/2026-10-02-m3-acceptance.md`).  Later images
  print `console: dynamic sio0.b OK` because the driver is then loaded
  from the recovery store.
- **M4 — Module loader (stage 07)**: linker-reserved bank-7 driver arena
  (`A000h-BFFFh`, 16 x 512-byte granules in the M4-era images; M8
  supersedes it with a linker-derived arena), packed ZMOD v1 driver
  containers, host `tools/zmodpack.py` (pack/validate/dump), a
  transactional first-fit kernel loader with relocation, BSS, device/IRQ
  claim rollback, and `module_code_allowed` handler validation, plus
  synthetic fixture modules proven by `make zmod-test`.  Hardware-validated
  on the physical Zephyr-80 2026-10-02 (`m4 loader selftest: OK`; see
  `docs/validation/2026-10-02-m4-loader-hardware.md`).
- **M4 — Dynamic driver acceptance (stage 08)**: the SIO driver is no
  longer linked into the kernel.  It is packed as a ZMOD module
  (`build/modules/sio.zmod`, plus IRQ and init-failure container
  variants), loaded through the generic loader at boot, and bound by the
  console service (`console: dynamic sio0.b OK`).  A placement image at a
  shifted arena base (9E00h-BE00h) used the identical module bytes, and a
  failure image proves exact rollback.  Hardware-validated on the physical
  Zephyr-80 2026-10-02 after fixing a CTC0 start/claim race
  (`console: dynamic sio0.b OK`, `m4 selftest: sio at 0xA000`; placement
  run `sio at 0x9E00`; failure run `m4 failure selftest: OK`).  Those M4
  images and targets were retired under the Mn-1 retention rule (see the
  retention note below); see `docs/module-format.md` and
  `docs/validation/2026-10-02-m4-acceptance.md` (historical record).
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
  ordinary ZEX in ROMFS, the `SYS_DIAG_*` recovery boundary, the shared
  user-space program support that became the SDK (`user/sdk/`,
  `docs/sdk.md`), the temporary pre-shell launcher, and
  the interactive monitor (`docs/monitor.md`).  Host-validated and
  physically accepted 2026-10-03
  (`docs/validation/2026-10-03-m7-monitor.md`).
- **Milestone retention (start of M9)**: the M1-M7 acceptance images,
  build targets, smoke modes and production hooks were retired under the
  Mn-1 retention rule (work package 25a); their still-relevant mechanism
  coverage (syscall gate, IRQ executive, ROMFS reader, process/ZEX and
  loader) moved to the subsystem acceptance image
  (`make subsystem-selftest`, `tests/test_subsys_sim.py`).
- **M8 — Native device stack**: driver ABI (v3) with a linker-derived
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
- **M9 — Native VFS and filesystem framework**: the kernel VFS core (path
  normalization, flat first-component mount selection, path-based provider
  registry, four-phase lifecycle, kernel file/directory API, `zmod_source`
  adapter), the generated static mount table (`config/mounts.json` +
  `tools/mountgen.py`), ROMFS as a statically linked filesystem provider
  mounted at `/SYSTEM`, the global owner-tagged descriptor pool and
  per-process CWD, the pinned native `FS_*` syscall family (`10h-1Fh`), the
  user SDK and its `fstest.zex` acceptance program, driver ABI v3
  `fs_register`, the deferred `ZDEV_BLOCK` class, `SYS_EXEC` nesting with
  the startup block v1 argument contract, and the resident IOC-FAT provider
  (`iocfat.zmod`) mounted at `/` from `ioc.0` with its one-slot
  write-through 512-byte file-data cache.  The milestone's end-to-end
  acceptance is `make m9-selftest` on a card; the full matrix, the
  tree/image hashes and the final bank-7 ledger are in
  `docs/validation/2026-10-05-m9-filesystem.md`, and the implementation
  record is `docs/vfs.md`.
  **Status: see the M9 acceptance matrix in
  `docs/validation/2026-10-05-m9-filesystem.md`** (the milestone closes only
  when every required row has passed).

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
make subsystem-selftest
                   # subsystem acceptance: the current boot path with an
                   # SIO-only boot set + loader/ROMFS/process fixtures,
                   # replaying the migrated syscall/IRQ/ROMFS/process/
                   # loader mechanism groups and the monitor session
                   # (build-subsys-selftest/)
make romfs-corrupt-selftest
                   # corrupt-recovery-store image: the kernel rejects the
                   # store and panics before any module runs
                   # (build-romfs-corrupt/)
make zmod-test
                   # build/pack/validate the SIO module variants and the
                   # synthetic ZMOD fixtures; host packer tests
                   # (build/modules/)
make romfs-test
                   # validate/dump the ROMFS recovery store; host format
                   # and reader tests
make zex-test
                   # pack/validate zextest.zex + the ZEX malformed matrix
make monitor-test
                   # pack/validate monitor.zex + ABI/layout/dependency tests
make ioc-test
                   # pack/validate ioc.zmod + host transport contract suite
make hid-test
                   # pack/validate hid.zmod + host class/artifact suite
make v9958-test
                   # pack/validate v9958.zmod + font pipeline + renderer
                   # model suites
make m8-ioc-selftest
                   # M8 IOC acceptance image + host replay
                   # (build-m8-ioc-selftest/)
make m8-hid-selftest
                   # M8 HID acceptance image + host replay
make m8-v9958-selftest
                   # M8 V9958 acceptance image + host replay
make m8-console-selftest
                   # M8 HID/V9958 console acceptance image + host replay
make m8-selftest
                   # M8 integration acceptance: full four-module image +
                   # package-20 target diagnostic + monitor launcher, and
                   # the production-byte simulator replay (boot order,
                   # binding, IOC command/bulk, HID queue, scripted
                   # monitor session, post-fault recovery)
                   # (build-m8-selftest/)
make m9-vfs-selftest
                   # M9 VFS bootstrap acceptance: /SYSTEM mounted by VFS,
                   # modules + monitor loaded by path, target VFS selftest
                   # (build-m9-vfs-selftest/)
make m9-required-mount-selftest
                   # M9 mount-policy acceptance image + host replay
make m8-fallback-selftests
                   # builds every failure/fallback variant image (no HID,
                   # no HID+V9958, malformed IOC, arena exhaustion, font
                   # failure, HID rollback) and replays each against its
                   # exact binding and early-SIO diagnostics
make m9-provider-selftest
                   # M9 provider ABI acceptance: the test-only writable
                   # RAM provider registered through ABI v3
make m9-fs-selftest
                   # M9 filesystem syscall acceptance: the native
                   # fstest.zex over the public FS_* family
make m9-exec-selftest
                   # M9 SYS_EXEC acceptance: nesting, argument transport and
                   # the depth probe
make m9-iocfat-selftest
                   # M9 IOC-FAT provider acceptance: the resident FS2
                   # provider against the simulator's FatFs peer
make m9-selftest
                   # M9 integration acceptance: the production boot path with
                   # `/` on the card, running fstest.zex (and its deep-matrix
                   # child fsfat.zex), the fsdeep.zex depth probe and the two
                   # fault windows (build-m9-selftest/)
# Hardware smoke checks: each mode verifies ONE specific image, and the
# production image and the acceptance images have different serial
# transcripts.  Build the image (`make hardware-images` builds them all),
# FLASH IT, then run the mode.  Each smoke target prints the exact image and
# sha256 to flash, and refuses to run when that image is missing.
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0
                   # flash build/zephyros.bin (make)
                   # capture + verify the production boot contract from
                   # physical hardware: the exact banner, the
                   # 'console: dynamic sio0.b OK' service line and the
                   # monitor banner + 'Z80> ' prompt (optional boot-module
                   # diagnostics are tolerated; a panic or an unbound
                   # console fails)
make smoke-hardware-m9 SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m9-selftest/zephyros-m9-selftest.bin
                   # (make m9-selftest): the whole M9 integration
                   # acceptance on a writable FAT card
make smoke-hardware-subsystem SERIAL_PORT=/dev/ttyUSB0
                   # flash build-subsys-selftest/zephyros-subsys-selftest.bin
                   # (make subsystem-selftest): the five group
                   # '... selftest: OK' lines and the interactive monitor
                   # session (sends 'Q', expects 'monitor: exit status 0x')
make smoke-hardware-romfs-corrupt SERIAL_PORT=/dev/ttyUSB0
                   # flash build-romfs-corrupt/zephyros-romfs-corrupt.bin
                   # (make romfs-corrupt-selftest):
                   # 'PANIC: romfs: corrupt recovery image'
make smoke-hardware-m8-ioc SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-ioc-selftest/zephyros-m8-ioc-selftest.bin
                   # (make m8-ioc-selftest): contract level, PING, bulk
                   # READY/DONE sections and the frozen no-file status
make smoke-hardware-m8-hid SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-hid-selftest/zephyros-m8-hid-selftest.bin
                   # (make m8-hid-selftest): the queue/doorbell transcript
make smoke-hardware-m8-v9958 SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-v9958-selftest/zephyros-m8-v9958-selftest.bin
                   # (make m8-v9958-selftest): the video transcript
make smoke-hardware-m8-console SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-console-selftest/zephyros-m8-console-selftest.bin
                   # (make m8-console-selftest): the serial portion
make smoke-hardware-m8 SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-selftest/zephyros-m8-selftest.bin
                   # (make m8-selftest): the integration image's serial
                   # portion (binding report, PING level, rejected-call
                   # invariants, 'm8 selftest: OK'); then perform the
                   # human HID/V9958 monitor pass on the display
make smoke-hardware-m8-fb-nohid SERIAL_PORT=/dev/ttyUSB0
                   # flash build-m8-fb-nohid/zephyros-m8-fb-nohid.bin
                   # (make m8-fallback-selftests); fallback variants (also
                   # -fb-nohid-nov9958, -fb-badioc, -fb-arena) each verify
                   # their exact binding, early-SIO diagnostic and scripted
                   # serial input where the input is SIO; V9958-bound output
                   # is confirmed on the display by the operator
make hardware-images
                   # build every image the smoke modes need
make clean         # remove generated files under build*/
```

Every build automatically runs `tools/mapcheck.py` (authoritative
post-link layout check) and `tools/pack_image.py` (byte-level image
validation, page packing, `build/zephyros.manifest.json` with sizes,
hashes, fill counts, and entry addresses).  `make check` additionally
proves that a clean rebuild reproduces the identical binary.

Physical-hardware validation uses `tools/serial_smoke.py` (115200 8N1,
bounded timeout).  Build the image a mode checks (the entry above names it;
`make hardware-images` builds them all) and flash it before starting the
mode -- the smoke target itself prints the image path and sha256.  The
subsystem and M8 modes must be listening before the board boots: start the
tool, then power-cycle or press RESET (the boot-log lines are printed once;
the M8 images additionally wait for the tool's host-sync sequence before the
banner).  The `subsystem`, `romfs-corrupt`, `m8` and `m8-fb-*` modes assert
the exact acceptance transcript, binding and diagnostics, scripting the
serial input where the bound input is SIO; V9958-bound output is confirmed
on the display by the operator.  Note: `smoke-hardware` (mode `normal`)
reads the console-service line and the monitor prompt from the serial link,
which only happens on an SIO-fallback bench (HID/V9958 absent).  On a full
bench the console binds HID input / V9958 output, so those appear on the
display and only the banner is on the serial link -- use
`smoke-hardware-m8` plus the human display check there.  Dated acceptance
records live under
`docs/validation/`
(M1, M2, the M3 record, the M4 loader/acceptance records, M5-M7, and
the package-20 M8 device-stack record; those retirements are recorded by
work package 25a).

Artifacts (all under `build/`):

```text
zephyros.ihx   linker output
zephyros.map   symbol/area map
page0.bin ... page7.bin   the eight 64 KiB ROM pages
zephyros.bin   512 KiB boot image (pages 0-7, FF-filled)
zephyr80.bin   identical copy for MAME
recovery.romfs logical ROMFS recovery stream (pages 2-7 payload)
modules/       packed ZMOD modules (+ dump/report JSON); every boot set
               is sourced from the ROMFS recovery store
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
checklist.  Expected output (the current boot then adds the dynamic
console-service line):

```text
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
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
│                          romfs-format.md, zex-format.md, monitor.md,
│                          sdk.md, vfs.md
├── config/mounts.json     production VFS mount policy (only /SYSTEM)
├── include/zephyros/      types.h, platform.h, kernel.h, syscall.h,
│                          errno.h, irq.h, device.h, driver.h,
│                          console.h, module.h, romfs.h, process.h,
│                          zex.h, ioc.h, hid.h, video.h, fs.h, vfs.h
├── fs/romfs/              M9 ROMFS VFS provider (romfs_provider.c)
├── boot/start.asm         common boot entry + C runtime initialization
├── boot/shadow_copy.asm   reset/ROM-to-SRAM copier
├── kernel/                main.c, panic.c, early_console.c, syscall.c,
│                          memory/ (banking.asm, user_copy.asm),
│                          syscall/ (entry.asm, page_zero.asm, diag.c,
│                          diag_io.asm), runtime/ (sdcc_helpers.asm),
│                          device/ (registry.c, console.c),
│                          irq/, module/ (arena.asm, loader.c,
│                          relocate.c),
│                          romfs/ (romfs.c, rom_access.asm),
│                          vfs/ (mount_config.h, path.c, mount.c, file.c,
│                          dir.c),
│                          process/ (process.c, exec.c, launch.asm)
├── drivers/               sio/ (sio.c, sio_io.asm, sio_module.c),
│                          ioc/, hid/, v9958/ (M8 device modules),
│                          ctc/ (ctc.c)
├── modules/               manifests/ (production ZMOD metadata),
│                          runtime/ (shared module-local SDCC glue)
├── platform/zephyr80/     constants/configuration only: hardware.h/.inc,
│                          memory.h/.inc, irq.h/.inc, config.mk
├── linker/zephyr80.lk     sdldz80 control template (derived _DATA base
│                          substituted into build/zephyr80.lk)
├── abi/zos/               the public native ABI (types, errno, syscall,
│                          fs, process, diag) -- one copy, all builds
├── user/                  sdk/ (crt0, libzos, zlibc, zex.mk -- the only
│                          way a native program is built),
│                          monitor/ (native monitor.zex sources + its
│                          hex/dump/status helpers),
│                          helloworld/ (bootable SDK demo)
├── recovery/manifest.json production ROMFS contents (sio.zmod, ioc.zmod,
│                          hid.zmod, v9958.zmod, v9958-6x8.font,
│                          monitor.zex)
├── tools/                 pack_image.py, mapcheck.py, mountgen.py,
│                          zmodpack.py, romfspack.py, zexpack.py,
│                          fontpack.py, serial_smoke.py
├── tests/                 host-side image/layout/ABI tests + Z80 replays;
│                          m8/, m9/, subsys/ (manifests, fixtures,
│                          generators); modules/ (test ZMOD sources +
│                          manifests); target/ (subsystem and M8/M9
│                          acceptance code, headers, shared runner)
└── work/                  milestone worker contracts
```
