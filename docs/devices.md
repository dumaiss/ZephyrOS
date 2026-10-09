# ZephyrOS Device, Driver, and Console Model (through M10 services)

This document describes the implemented device layer.  M10 (work package
41) replaced the M4-M9 model - drivers as bank-7 ZMOD modules registered
in a device registry, with a console device and console syscalls on top -
with **resident services**: the SIO0/B serial, IOC command+bulk
transport, HID keyboard/text and V9958 video driver stacks each run in
bank 6 as a service image, the console is a fifth service that composes
them, and the SDK and the nucleus reach all of it through the versioned
protocols in `abi/zos/protocol.h` (section 12 below).  The device
registry survives only for the module loader's acceptance fixtures, the
console device and the `SYS_CONSOLE_*` syscalls are gone, and the driver
ABI (`docs/module-format.md`) now serves the one remaining arena module,
the IOC-FAT provider.  The architectural contract is
`docs/architecture.md` sections 20-22, 30.2-30.3 and 36.4-36.14; this
file records what the build actually places where and how it is
validated.

M3 introduced the model with every driver statically linked.  M4 replaced
the *source of driver code*: the SIO driver is packed as a relocatable
ZMOD module (`docs/module-format.md`) and loaded into the bank-7 driver
arena during boot.  M8 (work package 14) moves the container header to
driver ABI 2: capabilities are a 32-bit class-scoped mask, descriptors
and registry entries carry separate `char_ops` and `class_ops` pointers,
driver-to-driver discovery is read-only by class plus capabilities, and
the production arena is derived from the aligned end of the linked
kernel.  M8 (work packages 15-16) adds the IOC transport: a second
production ZMOD module loaded after SIO that speaks the deployed MCU
command/reply lane and its bulk read/write lane, with the strict
READY/DONE lifecycle, without any firmware change.  M8 (work package
17) adds the HID keyboard/text class: a third production ZMOD module
loaded after IOC that fetches translated USB-keyboard bytes only when
the semantic doorbell is asserted and registers the input-only
`hid.keyboard0`.  The M3 test-only static drivers remain retired.  The
M1-M7 acceptance images -- including the M3 and M4 images and the
pre-M5 embedded-catalog boot mode -- were retired under the Mn-1
retention rule (work package 25a); the registry, IRQ, loader and console
mechanisms they proved now run on the current boot path in the subsystem
acceptance image (`tests/target/{syscall,irq,romfs,process,loader}/`,
`tests/test_subsys_sim.py`).

## 1. Concepts

```text
driver      implementation code for a hardware family
device      one registered hardware instance
capability  operations a device can provide
service     OS-level binding that selects devices for a role
```

```text
SIO driver (drivers/sio)
    │
    └── device "sio0.b"
          ├── class: ZDEV_SERIAL
          ├── caps:  CHAR_IN | CHAR_OUT | CONSOLE_IN | CONSOLE_OUT
          └── char:  putc/write/poll/getc (polling)
```

A device may expose generic character operations (`char_ops`), a
class-specific interface (`class_ops`, type determined by `class_id`),
or both.  Capabilities are authoritative: unsupported class operations
fail predictably rather than being inferred from the device name.  Class
values are stable: SERIAL=1, VIDEO=2, BLOCK=3 (formerly STORAGE), HID=4
(formerly INPUT), IOC=5 (formerly TRANSPORT), OTHER=6; capability ranges
are documented per class (generic 0-3, VIDEO 8-14, HID 16-18, BLOCK
20-23, IOC 24-27).

`ZDEV_BLOCK` is the generic raw block-device class.  M9 renames the
never-driven `ZDEV_STORAGE` numeric slot to `ZDEV_BLOCK` (the value 3 is
preserved, with no alias) and publishes its class contract in
`include/zephyros/block.h`: the `zblock_info` geometry, the `zblock_ops`
operations, and the `ZCAP_BLOCK_READ | ZCAP_BLOCK_WRITE |
ZCAP_BLOCK_FLUSH | ZCAP_BLOCK_REMOVABLE` capability bits in the
previously unused range 20-23.  M9 ships no resident `ZDEV_BLOCK` driver,
because nothing consumes one: IOC-FAT talks to `ZDEV_IOC` directly.  The
first block driver arrives with its first consumer, the first native
filesystem (`sd-ioc.zmod`, a `ZDEV_BLOCK` driver that consumes
`ZDEV_IOC`).

M9's resident filesystem provider is `iocfat.zmod`: loaded last through the
same VFS-backed loader (driver ABI v3 `fs_register`), it talks to the
`ZDEV_IOC` class interface only (`FS2` commands `30h-44h`, responses
`B0h-C4h`) and registers the `iocfat` provider, which the normal mount pass
attaches at `/` from `ioc.0`.  It owns a one-slot write-through 512-byte
file-data cache; `drivers/ioc/` was not changed by M9.  The acceptance image
is `make m9-selftest` (see
[`docs/validation/2026-10-05-m9-filesystem.md`](./validation/2026-10-05-m9-filesystem.md)).

A capability may be provided by different devices (a V9958 provides
VIDEO + CONSOLE_OUT; an HID device provides HID + CONSOLE_IN), so console
input and output bind independently.

## 2. Device registry

Fixed-size, heap-free, bank-7 kernel data
(`kernel/device/registry.c`, `include/zephyros/device.h` for the
module-facing types, `kernel/device/registry.h` for the kernel-private
lifecycle and storage):

- 16 entries; handles are the slot index + 1, issued in the range
  `1..16`.  Handle `0` is invalid.  Handles are valid for one boot and are
  not an external ABI.
- Each entry stores copied metadata (class, 32-bit capabilities, name
  pointer) and the driver-owned `char_ops`/`class_ops`/`state` pointers;
  the registry never absorbs driver-private state.
- Registration rejects null descriptors, null/empty names, descriptors
  with neither a character nor a class interface, out-of-range classes,
  duplicate instance names (`ZDEV_EBUSY`), and registry overflow
  (`ZDEV_ENOSPC`) without touching existing entries.
- Discovery is read-only:
  `device_find(class_id, required_caps, after)` returns the first
  registered device at or after cursor `after` whose class matches
  (`ZDEV_CLASS_ANY` = any) and whose 32-bit mask contains every required
  bit; zero `required_caps` and invalid classes/cursors yield handle `0`.
  `device_interface(handle, expected_class, &class_ops, &state)` verifies
  the live handle, the class, and a non-null class interface; it returns
  `ZDEV_EINVAL` (invalid handle/class/null outputs), `ZDEV_ENODEV`
  (stale/freed slot), or `ZDEV_ENOTSUP` (class mismatch or no class
  interface).  No client can mutate the registry.
- Registry mutation runs with maskable interrupts disabled through the M2
  executive lock (`irq_arch_lock`/`irq_arch_unlock`).

The module-facing `include/zephyros/device.h` carries only the ABI types,
the descriptor and registry-entry layouts, the class/capability values,
and the read-only discovery calls.  The lifecycle
(`device_registry_init`, `device_get`), the registration transaction
(`device_registry_checkpoint`/`_rollback`), the owner token, and the
bank-7 `device_table` storage are declared in the kernel-private
`kernel/device/registry.h`; a ZMOD module reaches registration only
through `zdriver_api.device_register`.

### 2.1 Registration transactions

Each driver initialization is wrapped in a registration transaction:

```c
zdev_checkpoint_t checkpoint = device_registry_checkpoint();
result = driver->init(&api);
if (result != ZDRIVER_OK)
        device_registry_rollback(checkpoint);
```

`device_registry_checkpoint()` snapshots the registration generation;
`device_registry_rollback()` removes exactly the entries created after
that snapshot (wrap-safe generation comparison) and returns how many were
removed.  Entries registered by earlier drivers are never touched.  This
is the same transaction boundary M4 reuses for module loads.

## 3. Driver contract (module API)

`include/zephyros/driver.h`:

```c
#define ZDRIVER_ABI_VERSION 2u

struct zdriver_api {
        int (*device_register)(const struct zdevice_desc *, zdev_handle_t *);
        int (*irq_claim)(irq_source_t, irq_handler_t, void *);
        int (*irq_release)(irq_source_t);
        zdev_handle_t (*device_find)(uint8_t class_id,
                                     zdev_caps_t required_caps,
                                     zdev_handle_t after);
        int (*device_interface)(zdev_handle_t, uint8_t expected_class,
                                const void **class_ops, void **state);
        int (*resource_size)(const char *name, uint16_t *out_size);
        int (*resource_read)(const char *name, uint16_t offset, void *dst,
                             uint16_t length);
};
```

The callback order and structure layouts are pinned by compile-time
assertions and host tests.  The two discovery calls expose no registry
mutation; the two resource imports resolve only ROMFS `DATA` entries,
bound every read, and never hand out `romfs_file_t` or mutable kernel
state.

`driver.h` and its class headers are the complete module-facing
compile-time surface.  They expose the ABI types and callbacks a driver
uses but no kernel-only declaration: the device registry lifecycle and
storage (`kernel/device/registry.h`), the IRQ-executive lifecycle,
claim transaction, diagnostics, platform hooks and interrupt-state lock
(`kernel/irq/irq_private.h`), and the raw memory-latch/bank primitives
(`kernel/memory/banking.h`) are all kernel-private.  Hardware-owning
driver implementations include the narrow `platform/zephyr80/`
constant header they require; `zephyros/platform.h` remains the
kernel's declarative constant aggregator.  `tests/test_source_layout.py`
resolves the module-facing include closure and compiles a representative
module against only those headers to keep the boundary mechanical.

- Init convention: `ZDRIVER_OK` (0) or `ZDRIVER_FAIL` (-1).
- This API is the interface the M4 loader passes to a loaded ZMOD
  module.  The M3 static driver table (`struct zdriver`,
  `boot_drivers[]`, `driver_init_static()`) is **retired**: no static
  driver table is linked into production images, and
  `tools/mapcheck.py --production` rejects any acceptance or
  static-driver symbol that would creep back in.
- Drivers are not offered any other kernel service: they never touch `I`,
  the IM2 page, the memory mode, the IRQ stacks, or `RETI`; those belong
  to the M2 executive.

Boot order in `kernel_main()` (after the M1/M2 stages):

```text
irq_init()
banner (early console)
[TEST-ONLY HOOK: zos_target_test_early() in acceptance builds]
device_registry_init()
module_loader_init()
boot module source (kernel_boot_module()):
    vfs_init()
      (register built-in providers, mount /SYSTEM from the "recovery"
       source; a failure prints the M5 romfs_diagnose() lines and
       panics with "romfs: corrupt recovery image")
    vfs_open("/SYSTEM/sio.zmod")   -> module_load_source()
    vfs_open("/SYSTEM/ioc.zmod")   ; M8, gated by ZOS_IOC_BOOT
    vfs_open("/SYSTEM/hid.zmod")   ; M8, gated by ZOS_HID_BOOT
    vfs_open("/SYSTEM/v9958.zmod") ; M8, gated by ZOS_V9958_BOOT
    vfs_mount_all()  ; no-op in production
      (failure: "<name>: unavailable 0x<ZOS_E*>"
       on the early console -- the exact code names a
       missing entry 0x05, a malformed container 0x0A,
       an exhausted arena 0x06 or a refused init 0x0B;
       the SIO fallback and every earlier resident
       module are untouched)
console_bind_defaults()       ; one final call, after all four loads
dynamic console line via console service
[TEST-ONLY HOOK: zos_target_test_late() in acceptance builds]
monitor launcher (unchanged process_exec("/SYSTEM/monitor.zex"))
halt loop
```

Acceptance code is selected only under `tests/target/support/`; the two
hooks above are the entire production-side integration surface.

## 4. SIO0/B module driver

`drivers/sio/` (`sio_io.asm`, `sio.c`, `sio_module.c`, `sio.h`, plus the
production manifest `modules/manifests/sio.json` and the shared
module-runtime glue `modules/runtime/sdcc_indirect_calls.asm`):

- The driver code is built, packed, and loaded from the recovery store as
  a ZMOD module; no SIO symbol is linked into the kernel image (enforced
  by `mapcheck.py`).
  Module sources must avoid byte-fragmented relocations, so the device
  descriptor is a `static const` and the module provides its own
  `___sdcc_call_iy/hl` glue (`docs/module-format.md` section 6).
  `sio_module.c` is production-only; the IRQ and init-failure container
  fixtures live under `tests/modules/` (`sio_irq_module.c`,
  `sio_fail_module.c`) with their own manifests.
- Ports: data `22h`, control `23h`; 115200 8N1, polled, interrupts stay
  disabled (`WR1 = 00h`).
- Registers exactly one device named `sio0.b`, class `ZDEV_SERIAL`, with
  `ZCAP_CHAR_IN | ZCAP_CHAR_OUT | ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT`.
- Init explicitly re-establishes the documented programming sequence
  (`18h, WR4=44h, WR3=C1h, WR5=EAh, WR1=00h`); it is idempotent with the
  early console's setup.
- Operations are bounded polling:
  - `putc` waits for TX-empty with a finite budget
    (`ZOS_SIO_TX_TIMEOUT`) and returns `ZDEV_ETIMEDOUT` on expiry;
  - `write` returns the number of bytes written (partial count on a
    timeout after the first byte);
  - `poll` returns 1/0 for RX-available;
  - `getc` returns the byte, or `ZDEV_EAGAIN` when nothing is pending.
- Driver-private state lives in the module's zero-initialized allocation
  (relocated `_DATA`); the registry only holds the relocated pointer.

The early console (`kernel/early_console.c`, common memory) remains a
completely separate, direct access path used by boot and panic.  Panic
output never depends on the registry, the driver, or the console service.

## 5. IOC command + bulk transport driver (M8)

`drivers/ioc/` (`ioc.c`, `ioc_io.asm`, `ioc_module.c`, `ioc_driver.h`,
`ioc_proto.h`; manifest `modules/manifests/ioc.json`) is the second
production ZMOD module.  It implements the deployed Zephyr-80 host
transport contract (level 08h) over both SIO lanes -- SIO1/B for
command/reply and SIO1/A for bulk DATA -- without any IOC firmware
change, and registers `ioc.0` only after its bounded link bring-up
produced a CRC-verified matching reply.

- Client contract: `include/zephyros/ioc.h` is the class ABI;
  `include/zephyros/ioc_protocol.h` is the client-visible service
  vocabulary -- the deployed MCU command/reply classes and DATA payload
  offsets (PING, HID input fetch) a protocol-aware driver or target
  diagnostic may share.  `drivers/ioc/ioc_proto.h` is transport-private
  and extends that vocabulary with the READY/DONE bulk constants, the
  frozen bulk-arming class gate, and the controller status bytes; no
  production driver other than the IOC driver includes it.  A driver
  reaches its own directory's private headers through same-directory
  quote includes, never through a shared `-I drivers/<name>` module flag.
- Class and capabilities: `ZDEV_IOC` with
  `ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ | ZCAP_IOC_BULK_WRITE |
  ZCAP_IOC_STATUS`.  The architecture's operation order (`command`,
  `bulk_read`, `bulk_write`, `status`) is pinned by
  `include/zephyros/ioc.h`; `ZIOC_BULK_MAX` is 512 DATA bytes.
- Native API: `struct zioc_frame` is TYPE, STATUS, DATA length, and up
  to 26 DATA bytes.  The rolling sequence and the wire CRC are
  transport-private.  `command()` maps one frame to the exact
  `A5 5A LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC` packet
  (CRC-16-CCITT, `1021h`, preset zero, MSB-first, no final XOR) and
  validates the reply frame, CRC, sequence, and exchange type binding
  (`reply = request | 80h`, or the explicit unknown-class reply `FEh`).
  `status()` returns semantic bits only (`ZIOC_STATUS_HID_PENDING`)
  sampled from `/CTSB`; raw SIO/RR0 bits never leave the module.
  Transport results are the distinct negative `ZIOC_E*` codes
  (`EBADFRAME`, `EBADCRC`, `EBADSEQ`, `ETIMEDOUT`, `EBUSY`, `EINVAL`,
  `EIO`).
- Bulk READY/DONE lifecycle: a successful command reply to a request
  class the controller can use to arm the bulk lane, shaped exactly
  like a bulk READY (status 0, length 8, nonzero transfer id, a known
  direction, 1..512 DATA bytes), admits one bulk phase and stores the
  identity privately.  The class gate is required: the wire gives READY
  no independent tag and immutable non-bulk replies can be eight bytes
  long (an HID fetch whose `queued`/`dropped` header looks like
  id/direction/length, a PROFILE bulk-tx page), so shape alone must
  never change the phase.  While admitted, no other command is accepted
  and only `bulk_read()`/`bulk_write()` matching the READY direction
  and exact length run; everything else fails with `ZIOC_EBUSY` before
  any lane I/O.  A completed read returns to idle.  A completed write
  enters DONE_REQUIRED: only `CMD_XFER_STATUS` is accepted, and success
  requires the DONE record to report the same transfer id and controller
  success.  A failed or mismatched DONE is never reported as a
  completed write; it arms whole-link recovery instead.
- SIO programming happens once, in the module init.  The command lane:
  channel reset, `WR1 = 00h` (no interrupts/WAIT), External Sync x1
  (`WR4 = 30h`), `FFh` underrun fill (`WR7`), receiver disabled
  (`WR3 = C0h`), TX enabled with RTS inactive (`WR5 = 68h`).  The bulk
  lane: channel reset, `WR1 = 00h`, `WR4 = 30h`, `AAh` WR6/WR7 fill,
  receiver enabled without Hunt (`WR3 = C9h`), transmitter idle
  (`WR5 = E0h`).  A channel reset, receiver disable, or Enter Hunt
  destroys character synchronisation, so an ordinary transaction never
  issues one; the bulk lane enters Hunt only before its first verified
  packet and during explicit link recovery.
- Link bring-up: up to four `CMD_LINK_SYNC` attempts; each attempt
  clears the sync flags of BOTH lanes (the MCU's handler resynchronises
  command and bulk together) and re-arms local Hunt, and `ioc.0`
  registers only when the reply is CRC-verified and is
  `RSP_LINK_SYNC`.  A failure returns `ZDRIVER_FAIL`; the loader rolls
  back, no device is published, and the SIO fallback is unaffected.
  The same bounded sequence is the whole-link recovery path used after
  any bulk-lane fault.
- One command transaction: reject null/malformed/oversize frames
  without any lane I/O; wait for COMMAND_READY on `/DCDB` (bounded,
  with `WR0` Reset External/Status before each sample); stamp the
  rolling sequence; build the packet and CRC; preload `A5`, reset the
  EOM latch, assert RTS, stream the frame with interrupts masked, add
  two `FFh` trailers, release RTS by disabling TX, then arm the
  receiver before restoring the exact entry IFF.  The receive path
  clears the error latch without resetting the channel, drains at most
  the three-byte FIFO depth, uses Enter Hunt only until the first
  verified reply, scans a bounded window for the full `A5 5A` marker,
  captures and validates header/body/CRC/sequence, clears unused reply
  DATA, and leaves the receiver enabled.
- Bulk read (`bulk_read`): preserve the entry IFF across the whole
  transfer; clear the bulk error latch; Enter Hunt only while the bulk
  boundary is unproven; drain at most the FIFO depth plus one stale
  byte; assert RTS; admit on `/DCDA` after Reset External/Status; scan
  a bounded window for `A5 5A`; capture the five header bytes at wire
  speed with an INI-shaped loop; move the READY-authorized length with
  the proven INI payload loop (whole-transfer stall budget); consume
  the two trailer bytes without comparison (the deployed pair bypasses
  bulk DATA CRC on both sides); release RTS by disabling TX; confirm
  `/CTSA` deasserted; then validate the captured LEN/TYPE/SEQ/STATUS
  against the stored READY identity.  Only a fully validated read
  proves the bulk character boundary.
- Bulk write (`bulk_write`): build the header bound to the READY's
  TYPE and sequence before entering the lane; keep the receiver
  enabled; program `AAh` WR6/WR7 fill and arm the SIO TX CRC generator;
  preload the disposable `00h` lead-in before RTS; admit on `/CTSA`;
  send `A5 5A` and the header; stream the payload with the proven
  OUTI-shaped bounded loop; issue the EOM reset only after the final
  payload byte; wait for the transmitter underrun; retain the measured
  128-iteration CRC completion guard; release RTS; restore interrupts
  BEFORE the potentially long controller commit wait; then confirm
  `/CTSA` deasserted.  The MCU currently ignores the generated trailer,
  but the wire/timing behavior matches the proven implementation.
- Failure semantics: every timeout and malformed-packet exit releases
  RTS, restores the exact entry IFF, clears the busy guard and the
  admission state, and arms whole-link recovery; the next command
  re-establishes both lanes before normal traffic, so no stale READY
  can survive a fault.
- Serialization: a module-private foreground busy guard makes
  recursive/overlapping use fail with `ZDEV_EBUSY`; the M2 registry
  lock is never held over transport I/O.
- Boot integration: production images carry `sio.zmod`, `ioc.zmod`,
  `hid.zmod`, and `monitor.zex` in the ROMFS recovery store and load
  SIO first, then IOC, then HID, as part of the deterministic boot set
  reported by the arena plan.  A failed IOC load/bring-up prints
  `ioc: unavailable 0x<ZOS_E*>` and the console still binds to SIO.
  Package 20 adds the observable failure variants: the fallback images
  (`build-console-fb-nohid`, `-nohid-nov9958`, `-badioc`, `-arena`, plus
  the font-failure and HID rollback images) boot the same four-module
  sequence with one manifest/interface failure and are replayed
  against their exact bindings by `tests/test_console_fallback.py` (see
  section 8.2).  Earlier
  acceptance images compile the IOC and HID loads out (`ZOS_IOC_BOOT=0`
  / `ZOS_HID_BOOT=0`) and stay byte-identical to their accepted builds.
- Host validation: `tests/test_ioc.py` pins the platform constants of
  both lanes (including the explicitly paired bulk CRC bypass),
  class contract, module artifact, boot order, and mapcheck rules;
  `tests/test_ioc_sim.py` replays the real IOC acceptance image against
  the simulator's IOC command+bulk peer (exact wire bytes on both
  lanes, READY/DONE interlocks, one-time Hunt per lane, fault injection
  on both directions, bounded failure, whole-link recovery, canaries,
  IFF restoration, rollback, and semantic status).  The physical
  acceptance image is `build-ioc-selftest/` and is driven by
  `make smoke-hardware-ioc`; board timing remains a physical
  hardware authority.  The target diagnostic runs the full /SHARED
  READY/DONE acceptance (write 1/128/512, matching DONE records, read
  back and verify, delete the dedicated `IOCBULKT.BIN`, prove the delete
  by a reopen answered exactly with the frozen no-file status (34h) and
  the empty failure shape); only a verified unknown-command reply for the
  writable open may report the bulk-write criterion PENDING - both
  PENDING markers print exactly `0xFE` - and a PENDING run ends with its
  own explicit marker (never the OK marker, and the smoke tool exits
  nonzero without printing its OK result).  A real filesystem/media
  status is a failure.  The smoke image is built
  with `ZOS_SMOKE_HOST_SYNC=1`: instead of the blind connect delay it
  holds the banner until the host sends the two-byte sync sequence on
  SIO0/B (`ZOS_SMOKE_HOST_SYNC_FIRST/SECOND`), because this bench's
  reset line also resets the USB bridge, so the capture can only start
  once the host has reopened its port (`serial_smoke.py` sends the
  sequence on every open and repeats it until the first byte arrives).

## 6. HID keyboard/text driver (M8)

`drivers/hid/` (`hid.c`, `hid_module.c`, `hid_driver.h`; manifest
`modules/manifests/hid.json`) is the third production ZMOD module.  It
implements the native HID class (`include/zephyros/hid.h`) on top of the
IOC transport and registers exactly one input-only device after it has
discovered the IOC class; the MCU firmware is untouched.

- Class and capabilities: `hid.keyboard0`, class `ZDEV_HID`, with
  `ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN` and nothing else.
  `ZCAP_HID_KEY_EVENTS` and `ZCAP_HID_GAMEPAD` are reserved and absent;
  their shapes are frozen in `hid.h` so a later implementation can add
  the operation without changing the class boundary.  `read_gamepad`
  stays null, and an unsupported capability must be reported as
  `ZDEV_ENOTSUP` by a dispatcher, never inferred from the name.  M8
  implements only `ZCAP_HID_TEXT`.
- Dependency discovery is by class/capability/interface only:
  `device_find(ZDEV_IOC, ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS, ...)` plus
  `device_interface()`.  The module contains no imported IOC symbol and
  no name lookup; its only IOC compile-time surface is the public
  `include/zephyros/ioc_protocol.h` vocabulary, never the IOC driver's
  private headers.  A missing or malformed dependency (null command or
  status operation, class mismatch, no class interface) fails the init;
  the loader rolls the (empty) registration set back and the SIO
  fallback and IOC transport stay untouched.
- Input path: a 16-byte power-of-two local ring.  `poll`/`getc`/
  `read_text` report buffered bytes first; otherwise they sample the
  semantic `ZIOC_STATUS_HID_PENDING` doorbell and issue exactly one
  `CMD_HID_INPUT` only while it is asserted.  The request asks for the
  ring's exact free space (never above the protocol's 24-byte reply
  maximum) because the controller dequeues what it sends.  A reply is
  validated (transport success, `RSP_HID_INPUT`, controller status,
  2..26 DATA bytes, byte count within both the request and the ring
  capacity) before any byte is committed; a malformed reply leaves the
  ring unchanged.  Bytes are preserved verbatim, including NUL,
  control bytes and multi-byte VT100 key sequences.
- Failure policy: an asserted doorbell followed by a failed or empty
  fetch imposes the proven 64-call stuck-doorbell penalty before
  another IOC command; successful data or a released doorbell clears
  it.  A transport failure is reported as no input (0 / `ZDEV_EAGAIN`)
  with the penalty throttling retries, so a broken IOC link cannot wedge
  console polling.  HID never samples SIO1, RR0, CTS or any port; raw
  status stays an IOC implementation detail.
- Boot integration: production images carry `sio.zmod`, `ioc.zmod`,
  `hid.zmod`, and `monitor.zex` in the ROMFS recovery store and load
  SIO, then IOC, then HID; a failed HID load prints
  `hid: unavailable 0x<ZOS_E*>` and the console still binds SIO (the
  `build-console-fb-badioc` malformed-IOC and `build-console-fb-nohid`
  missing-entry variants prove both failure classes -- section 8.2).
  With HID registered, `console_bind_defaults()` selects HID input and
  the preferred V9958 output, or the SIO output fallback when the
  V9958 device is absent.
- Host validation: `tests/test_hid.py` pins the class contract,
  protocol constants, module artifact, boot order, source discipline
  (no port/status constants, no direct IN/OUT, discovery only through
  the driver API, exact reply validation), the fake-IOC fixture and the
  mapcheck rules; `tests/test_hid_sim.py` replays the real SIO+IOC+HID
  image against the simulator's IOC peer (scripted USB typing through
  the real `CMD_HID_INPUT` exchange, fetch-on-doorbell only, exact
  sizing, ring wraparound and control-byte preservation,
  malformed-reply atomicity, transport-error throttling, the 64-call
  stuck penalty, the eight-byte READY collision, the no-IOC rollback,
  and the malformed-IOC interface rejection).  The physical acceptance
  image is `build-hid-selftest/`, driven by
  `make smoke-hardware-hid`: the USB-keyboard pass is typed by the
  operator, the tool validates the echoed `hid: rx XX` transcript,
  and then sends the forced SIO-fallback byte.

## 7. V9958 video core (M8)

`drivers/v9958/` (`v9958.c`, `v9958_io.asm`, `v9958_module.c`,
`v9958_driver.h`, `assets/font_cp850_6x8.inc`; manifest
`modules/manifests/v9958.json`) is the fourth production ZMOD module:
the direct-hardware LunchCrema V9958 core ported from the pinned
Zephyr-80 reference.  It delivers the hardware-safe initialization, the
native VIDEO class operation surface, the ROMFS-backed font atlas, the
low-level graphics/command primitives, and (package 19) the ported 85x26
6x8 terminal renderer behind an output-only `zchar_ops` table, so the
same hardware serves graphics clients and the normal text console.

- Class and capabilities: `v9958.0`, class `ZDEV_VIDEO`, with exactly
  the seven `ZCAP_VIDEO_*` bits (control/data/block/palette/indirect/
  present/reset) plus `ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT` for the
  character renderer.  `char_ops` points at `v9958_char_ops`, whose
  `putc`/`write` entries feed one byte / the whole 16-bit length into
  the terminal parser and whose `poll`/`getc` entries are null (the
  device is output only); `console_bind_defaults()` therefore selects
  it as the preferred console output, while input comes from a separate
  device.
- Terminal behavior (ported from the pinned reference, hardware-proven
  layout): 85 logical columns, 26 rows, 6x8 glyph cells on a 208-line
  text surface with a four-line margin inside the circular 256-line
  page-zero bitmap, normal (10000h) and reverse (14000h) precolored
  atlases, circular scrolling by advancing R#23 in cell steps, and a
  sprite-mode-2 cursor whose SAT Y/X derive from the live cursor state.
  Printable CP850/ASCII bytes 20h-FFh print; NUL/BEL/SO/SI and every
  other control byte below 20h are consumed after flushing the pending
  print run; CR homes the column, LF/VT advance the row keeping the
  column, BS backs up one cell, TAB advances to the next 8-column stop,
  FF and `ESC[2J` clear the screen and home, and `ESC[K`/`ESC[0K`,
  `ESC[1K`, `ESC[2K`, `ESC[J`, `ESC[0J`, `ESC[1J`, `ESC[X` implement the
  erase families with the current SGR background.  `ESC[7m`/`ESC[27m`/
  `ESC[0m` drive reverse/normal video (other SGR parameters are
  consumed), `ESC[s`/`ESC[u` and `ESC 7`/`ESC 8` save and restore
  cursor and rendition, `ESC[?7h/l` control DECAWM (wrap-first at the
  right margin, otherwise clamp-and-overwrite), `ESC[?25h/l` show/hide
  the cursor, `ESC[A/B/C/D/G/d/H/f` move or place the cursor with
  reference clamping, `ESC[L`/`ESC[M` insert/delete lines, `ESC c` and
  the triple-ESC reset reinitialize terminal and display together, and
  unknown or partial sequences are consumed without writing outside the
  terminal state or the page-zero bitmap.  Print runs batch up to 64
  glyphs and flush before control operations, present and cursor
  updates so ordinary strings stay efficient and visible.
- Private renderer stack: the `zchar_ops` entries are assembly wrappers
  (`v9958_io.asm`) that switch to a private 256-byte module stack before
  entering the C workers, because a complete renderer call chain exceeds
  the common syscall gate stack depth that syscall-context callers run
  on.  The workers preserve the exact sdcccall(1) prototypes and return
  values; the gate stack itself is sized for the deepest accepted
  syscall chain (see `docs/memory.md`).
- Native operations (`include/zephyros/video.h`):
  `ZVIDEO_CTRL_WRITE` (1), `ZVIDEO_DATA_WRITE` (2),
  `ZVIDEO_DATA_BLOCK` (3), `ZVIDEO_PALETTE_WRITE` (4),
  `ZVIDEO_INDIRECT_WRITE` (5), `ZVIDEO_PRESENT` (6), `ZVIDEO_RESET`
  (7), dispatched through `zvideo_ops.send`.  The numbers are native,
  not Virtual Drip packet numbers.  `send` validates before any I/O:
  write operations require a non-null payload and a nonzero length and
  stream the full 16-bit length (native ZephyrOS does not inherit the
  historical 16-byte non-block ceiling); PRESENT/RESET require zero
  length; unknown operations return `ZDEV_ENOTSUP`, malformed requests
  `ZDEV_EINVAL`, and a null instance `ZDEV_ENODEV`.  Capabilities are
  authoritative.
- Hardware discipline (ports A0h-A4h; declarative ports, latch bits,
  register values and the bounded bootstrap delay live in
  `platform/zephyr80/hardware.{h,inc}`, paired and parity-tested): the
  latch shadow preserves D0 while the porch is disabled for the paced
  bootstrap; R#25 WTE=0/VDS=0, R#8=08h (64Kx4 DRAM organization), stop
  any prior command, then the full GRAPHIC 6 register/selector state
  (R#0=0Ah, R#2=1Fh, R#9=88h, R#15=2, R#23=100h-4, ...), then R#25=04h
  (WTE=1/VDS=0) and the porch; the palette is programmed only under
  native WAIT; both precolored font atlases upload, the circular
  page-zero bitmap clears to the 44h background, the sprite cursor
  initializes, the frame presents, and the display enables.
  `ZVIDEO_RESET` re-establishes every owned register and VRAM asset and
  preserves D0.  Status register 2 stays selected while the driver owns
  the VDP and command starts wait for CE completion.  `/INT` stays
  disabled: `IRQ_VDP_FF` is never claimed.
- Font: the reviewable asset `assets/font_cp850_6x8.inc` (the pinned
  2048-byte CP850 6x8 font; SHA-256 `6db207e0...`) is emitted
  deterministically by `tools/fontpack.py` as ROMFS DATA
  (`v9958-6x8.font`) and is never embedded in the resident module image
  or BSS.  The driver validates the resource size before any port I/O,
  streams it through the ABI-v2 resource imports in bounded 256-byte
  chunks, and converts each 32-glyph group into the normal and reverse
  atlases at 10000h and 14000h in the 128-KiB VRAM space.
- Boot integration: production images carry `v9958.zmod` and
  `v9958-6x8.font` in the ROMFS recovery store and load the module
  after HID (every console candidate is registered before binding).  A
  missing/undersized font resource or a failed VDP sequence fails the
  init, the loader rolls the (empty) registration set back, and the SIO
  fallback, IOC transport and HID remain untouched (`v9958:
  unavailable 0x<ZOS_E*>` on the early console; the font-failure image
  and the `build-console-fb-arena` allocation-exhaustion image cover both
  paths -- section 8.2).
- Host validation: `tests/test_v9958.py` pins the class contract, the
  paired platform constants, the font asset pipeline, the module
  artifact (including the console-capable descriptor and the private
  renderer-stack wrappers), the ROMFS boot set and the source discipline
  (all port I/O in `v9958_io.asm`; no CP/M tables, fixed BIOS addresses,
  Virtual Drip framing or IOC/HID imports; no interrupt claim;
  validate-before-I/O; the proven initialization order).
  `tests/test_v9958_sim.py` replays the real acceptance image against
  the simulator's register/VRAM-level V9958 peer: registration and
  binding, latch preservation, paced bootstrap ordering, the
  WAIT-enabled palette phase, the exact register sequence, both atlases
  byte-for-byte, CE command-completion waits, 128-KiB R#14 addressing,
  RESET, the deterministic visible pattern, malformed no-I/O cases, a
  4096-byte `DATA_BLOCK`, and the font-failure rollback.
  `tests/test_v9958_console.py` drives the module's `zchar_ops` and
  `ZVIDEO_RESET` with scripted byte streams and decodes the VDP command
  trace into a physical-bitmap screen model (glyph copies, fills, row
  copies, R#23 origin); fixed golden cases cover printable placement,
  CR/LF/VT/BS/TAB/FF, right-margin wrap and DECAWM off, cursor
  boundaries and CSI families, erase display/line/characters, SGR
  reverse video and reverse-background fills, save/restore, insert/
  delete lines, circular scrolling, cursor visibility, all reset paths,
  print-run batching, arbitrary write chunking, and exact command-block
  geometry.  The physical acceptance image is
  `build-video-selftest/`, driven by `make smoke-hardware-video`;
  the console image `build-console-selftest/` (HID input + V9958
  output) is driven by `make smoke-hardware-console` and by
  `tests/test_console_sim.py`; the operator confirms the documented pattern
  and the visible monitor session.

## 8. Console service

`kernel/device/console.c`, `include/zephyros/console.h`:

- Stores separate input and output handles; operations dispatch through
  the bound device's generic `struct zchar_ops`.
- `console_bind_defaults()` selects by class preference first and falls
  back independently per direction to any device carrying the generic
  console capability:

  ```text
  input   ZDEV_HID   + ZCAP_CONSOLE_IN, else any + ZCAP_CONSOLE_IN
  output  ZDEV_VIDEO + ZCAP_CONSOLE_OUT, else any + ZCAP_CONSOLE_OUT
  ```

  A missing direction never clears a valid other direction: whichever
  handles were found stay bound and the call returns `ZDEV_ENODEV` only
  when at least one direction is unbound.  SIO remains the
  both-direction fallback.
- `console_bind(input, output)` validates non-zero handles against the
  matching capability; either handle may be `ZDEV_HANDLE_INVALID` to leave
  that direction unbound.
- Operations pass through to the bound device:

| Call | Unbound | Op unsupported | Device error |
| --- | --- | --- | --- |
| `console_putc` | `ZDEV_ENODEV` | `ZDEV_ENOTSUP` | passed through |
| `console_write` | `ZDEV_ENODEV` | `ZDEV_ENOTSUP` | byte count / error |
| `console_poll` | `ZDEV_ENODEV` | `ZDEV_ENOTSUP` | 1/0 / error |
| `console_getc` | `ZDEV_ENODEV` | `ZDEV_ENOTSUP` | byte / `ZDEV_EAGAIN` |

Normal boot prints its acceptance line through this service (the device
is the dynamically loaded module):

```text
console: dynamic sio0.b OK
```

### 8.1 Console channels

Five distinct paths must not be confused:

- native VIDEO class writes (`zvideo_ops.send`): graphics/register
  operations to `v9958.0`, independent of any text mode.
- V9958 character output (`ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT`, the
  output-only `zchar_ops` table): the 85x26 terminal renderer, consumed
  by the console service and therefore by `console_putc`/`console_write`.
- HID character input (`ZCAP_CONSOLE_IN`): keyboard bytes fetched by the
  console service from `hid.keyboard0`, decoded by the HID module from
  IOC replies.
- SIO console fallback (`ZCAP_CONSOLE_IN`/`ZCAP_CONSOLE_OUT` on
  `sio0.b`): the both-direction fallback used when a preferred
  direction is unavailable; this is also the M7-normal binding.
- direct early SIO (`early_console_*`): the permanently resident,
  binding-independent path used by the boot banner, the target
  control transcripts and `panic()`.  It never consults the console
  service, so it works even when the bound pair is HID/V9958.

The console service composes one bound input with one bound output;
neither module knows about the other.  The acceptance image
`build-console-selftest/` binds HID input to V9958 output and runs the
unchanged M7 `monitor.zex` through that pair; `make
smoke-hardware-console` validates the early-SIO boot transcript while
the operator confirms the visible session.  The package-20 integration
image `build-console-integration-selftest/` binds the same pair, runs the package-20
target diagnostic, and is driven by `make smoke-hardware-console-integration` followed by
the human monitor pass (section 8.2).

### 6.1 Failure-path evidence

The retired M3 fallback flags (`ZOS_M3_NO_STATIC_DRIVERS`,
`ZOS_M3_NO_CONSOLE_BIND`) no longer exist, and the M4 failure image that
proved failed-init rollback on target was retired under the Mn-1
retention rule (work package 25a).  The rollback mechanism is now covered
by the loader group of the subsystem acceptance image
(`tests/target/loader/loader_test.c`, `tests/test_subsys_sim.py`), which
loads the failed-init fixture `tests/modules/m4_fail.c` from `/SYSTEM`
and checks exact device/IRQ/granule/descriptor rollback; the
`sio_fail`/`sio-irq` ZMOD fixtures remain packed containers for
`make zmod-test`.  The runtime fallback evidence is the M8
failure/fallback matrix in section 8.2:

| Scenario | Behavior | Early-console diagnostic |
| --- | --- | --- |
| runtime failure | an optional module's load/init failure is rolled back and diagnosed (`build-console-fb-*`) | `<name>: unavailable 0x<ZOS_E*>` |
| runtime failure | required SIO load/init reports failure | `console: dynamic driver failed` |
| runtime failure | binding finds no suitable device | `console: unbound` |
| runtime service failure | bound device reports an error | `console: service failed` |

Every production image keeps the full M0 banner and panic path.

### 8.2 Console integration acceptance (package 20)

Package 20 freezes the production device stack and adds its
integration gate:

- **Boot contract**: `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod`
  (plus `v9958-6x8.font` and `monitor.zex`) are the only production
  ROMFS entries and are loaded in that order through their
  `/SYSTEM/<name>.zmod` VFS paths (M9); a single
  `console_bind_defaults()` runs after all four loads, followed by the
  unchanged `process_exec("/SYSTEM/monitor.zex")` launcher.  A failed optional
  module prints `<name>: unavailable 0x<ZOS_E*>` on the direct early
  SIO (0x05 missing entry, 0x06 arena exhausted, 0x0A malformed
  container, 0x0B refused init) and the boot continues; ROMFS
  corruption keeps the fatal recovery policy.
- **Failure/fallback matrix** (each row asserts its exact binding,
  diagnostics and observable direction from the real linked bytes):

  | Image | Injected failure | Early-SIO diagnostic | Binding |
  | --- | --- | --- | --- |
  | `build-console-integration-selftest/`, `build/` | none (all four modules) | -- | HID/V9958 |
  | `build-video-fontfail-selftest/` | undersized ROMFS font | `v9958: unavailable 0x0B` | HID/SIO |
  | `build-console-fb-nohid/` | `hid.zmod` entry absent | `hid: unavailable 0x05` | SIO/V9958 |
  | `build-console-fb-badioc/` | fake IOC (null command op) fails HID init | `hid: unavailable 0x0B` | SIO/V9958 |
  | `build-console-fb-nohid-nov9958/` | HID and V9958 entries absent | both `unavailable 0x05` | SIO/SIO |
  | `build-console-fb-arena/` | `v9958.zmod` replaced by an oversized allocation | `v9958: unavailable 0x06` | HID/SIO |

- **Host/simulator evidence**: `make console-integration-selftest` builds the
  integration image (full stack + package-20 target diagnostic + monitor
  launcher) and replays the production bytes
  (`tests/test_console_acceptance.py`): boot order and binding, IOC
  command/status/repeated-PING and the /SHARED READY/DONE bulk
  interlock, HID queue traffic with the stuck-doorbell penalty, the
  scripted monitor session with two Q exit/relaunch cycles,
  malformed/null/class/capability rejections with device-table,
  granule-allocation and lane-handshake invariants, and post-fault
  recovery.  `make console-fallback-selftests` builds every variant above
  and replays `tests/test_console_fallback.py`; `tests/test_driver_abi.py`
  reconciles the aligned linked-kernel end, derived arena, arena plan
  and image manifest, and checks every subsystem ROMFS manifest's
  content/types/order.
- **Physical gate**: flash `build-console-integration-selftest/zephyros-console-integration-selftest.bin`,
  run `make smoke-hardware-console-integration` before reset, perform the human
  HID/V9958 monitor pass, then run each `make smoke-hardware-console-integration-fb-*`
  mode on its image and record what was observed on the display.  The
  package-20 validation record
  (`docs/validation/2026-10-03-m8-device-stack.md`) carries the exact
  image hashes; all physical items were observed on the 2026-10-04
  bench (serial smoke, the full monitor pass — including the `R`
  screen and the rejected-input `ERR: ARG` lines — the four fallback
  runs and the operator's display confirmations for the V9958-bound
  variants), and M8 is marked complete.

## 9. Drivers and interrupts

A loaded module may claim an M2 logical source through
`zdriver_api.irq_claim` and receives a bank-7 handler; the executive maps
bank 7 before dispatch and owns the final `RETI`.  The production SIO
module stays polling-only.  Two current paths prove the IRQ integration
on the subsystem acceptance image (`tests/test_subsys_sim.py`):

- the IRQ group (`tests/target/irq/irq_test.c`) drives the executive
  directly: registry semantics (invalid source/handler, duplicate claim,
  release, double release), mode-10 delivery from application banks 0
  and 6, mode-11 delivery on the kernel stack with mode-mapping and
  bank-7 ISR-stack evidence, and the unclaimed-source quiesce path;
- the loader group's relocation fixture (`tests/target/loader/`,
  `tests/modules/m4_probe.c`) claims `IRQ_CTC0` from its *relocated* init
  and proves the claim is owned by the module.  The SIO IRQ variant
  (`tests/modules/sio_irq_module.c`, `tests/modules/sio-irq.json`)
  remains a packed container fixture for `make zmod-test`.

The CTC0 programming these paths need is test-owned
(`tests/target/include/ctc_test.h`, `tests/target/support/ctc_test.c`)
and is linked only into the subsystem acceptance image.  The production
CTC owner (`drivers/ctc/ctc.c`) keeps only `platform_irq_quiesce()`;
`platform_ctc0_test_start()`, `platform_ctc0_stop()`, and the obsolete
`platform_irq_vector()` are not declared or linked in production.

## 10. Placement and validation

All driver/service code, descriptors, ops tables, and state live in
bank 7 (linked `_CODE`/`_DATA` for the remaining kernel objects; the
loaded module inside the derived driver arena).  None of it may enter
permanent common memory.  Production derives the arena start from the
aligned linked-kernel end and the arena end from the runtime-data size
(`data_base = align_down(DF00h - data_size, 512)`), with at least 16 KiB
remaining; the fixed `A000h-BFFFh` geometry of the earlier acceptance
images is retired (work packages 25a/25b), so there is exactly one
geometry (see `docs/memory.md` section 2.2 and `docs/module-format.md`
section 1).
`tools/mapcheck.py` enforces this with an explicit symbol list and a
prefix sweep over `_device_`, `_console_`, and `_m3_` symbols.  A
rejection sweep fails the build on any linked `_sio_`, `_sio0b_`,
`_sio_driver`, `_ioc_`, `_hid_`, or `_v9958_` symbol (the SIO, IOC, HID
and V9958 drivers may exist only as modules), and
`mapcheck.py --production` additionally rejects every acceptance symbol
(static-driver remnants, test runners/results, fixture catalogs, test
SIO module entries) in the production image.

Host validation layers:

- `tests/test_driver_abi.py` – driver ABI version/structure/order and capability-range
  contract, derived-arena geometry (alignment, 16-KiB minimum, unaligned
  relation, linked bytes in the arena), the boot-set arena plan and its
  overflow rejection, ABI v1 rejection, and the architectural granule
  bound.
- `tests/test_zmod.py`, `tests/test_source_layout.py` – the ZMOD
  container/relocation contract (including a two-base relocation check
  and bad version/ABI rejection), the module-facing include closure, and
  the milestone-retention window (only M9/M10 acceptance machinery may
  exist).
- `tests/test_subsys_sim.py` – replays the subsystem acceptance image in
  the Z80 simulator: the syscall and IRQ groups at the early hook, the
  ROMFS-reader, process/ZEX and loader groups at the late hook (with
  injected CTC0 vectors and loader arena exhaustion), and the scripted
  monitor session.
- `tests/test_romfs_sim.py` – the ROMFS reader/access replay and the
  corrupt-store image (`TestCorruptImageReplay`: panics with
  `PANIC: romfs: corrupt recovery image` before any module runs).
- `tests/test_hid.py` / `tests/test_hid_sim.py` – the HID class/driver
  contract and the real SIO+IOC+HID image replay (section 6).
- `tests/test_v9958.py` / `tests/test_v9958_sim.py` – the V9958 VIDEO
  class/driver contract, the font asset pipeline, and the real
  SIO+IOC+HID+V9958 image replay against the V9958 peer (section 7).
- `make subsystem-selftest` / `make romfs-corrupt-selftest` – the
  migrated mechanism acceptance image and the corrupt-store image.
- `make smoke-hardware-subsystem SERIAL_PORT=...` – driving the subsystem
  image: the five group `... selftest: OK` lines and the interactive
  monitor step (the tool sends `Q` and expects `monitor: exit status 0x`).
- `make smoke-hardware-romfs-corrupt SERIAL_PORT=...` – capture + verify
  the corrupt-image panic marker.

SIO0/B timings, CTC0 delivery, console RX, and the IOC command-lane
and bulk-lane transports remain physical-hardware authorities; the
simulations cover control flow and register-level behavior.  HID input
arrives from the deployed MCU translation; physical USB-keyboard
acceptance is typed by the operator into the smoke run.  The migrated
subsystem and corrupt-store images, and the changed production image
(the banner and hooks changed), are hardware-`PENDING` (work package
25a).

## 11. Out of scope

No heap, no module unload or hot-plug, no filesystem API for modules
(beyond the two bounded DATA resource imports), no process API here, and
no user-visible device syscall.  M9 defines the `ZDEV_BLOCK` class
contract without a resident driver: the first block driver arrives with
its first consumer (architecture 32.15/32.39).  M4 stages 07-08 (this
document plus `docs/module-format.md`) delivered the loader and the
relocatable SIO driver module; M8 work package 14 delivered driver ABI
v2 (raised to v3 by M9 package 26), the read-only discovery calls, the
per-class capability ranges, the preferred-direction console policy, and
the derived production arena;
M8 work packages 15-16 delivered the IOC command+bulk class and its
deterministic boot load after SIO; M8 work package 17 delivered the HID
keyboard/text class, its boot load after IOC, and the input-only
`hid.keyboard0` device; M8 work package 18 delivered the V9958 video
core: the `v9958.0` VIDEO device with the seven native operations, the
ROMFS-backed font atlases and the command/VRAM primitives, loaded after
HID (character output and the terminal remain with the console
package).  The M3 numeric registry model still holds; its placeholder
class names were refined in place.

## 12. Resident driver protocols (M10, packages 41-41b)

The five driver images the production store carries are the SIO serial
transport, the IOC transport, HID input, the V9958 video core and the
console.  Each is a ZMOD driver image (`docs/module-format.md`, service
ABI 3, host-linked at its final bank-6 address) placed by the resident
loader (`service_load_all`, manifest `services.man`); the host packer
assigns each endpoint number and resolves the console's and HID's
symbolic dependencies, the loader installs the host-built tables, and each
image is entered only through `struct ipc_service_boot` (the nucleus table
plus the resolved dependency numbers) - a driver never calls another
driver's symbols and never looks a peer up at run time.  `abi/zos/protocol.h`
is the whole contract; this section records the operation numbers, frames
and descriptor conventions the images implement.

Common shape.  Every operation is one `call(handle, op, frame)`:
`frame->arg0`/`arg1` are the operation's two words, `frame->payload` is
8 inline bytes, `frame->desc` is the nucleus-issued descriptor id (or
`ZOS_IPC_DESC_NONE`), and `frame->status` carries the result -0 or a
negative `ZOS_E*`/class error.  A descriptor-bearing operation also
passes `arg0` = offset into the caller's buffer and `arg1` = length, so
one call moves one bounded run.  Operations that return a single word
(the status queries and the byte-sized forms) publish it as the call's
return value.

Shared-memory staging.  A driver that has bytes to hand on names its own
service-window buffer in the reply payload; the peer or the nucleus
finishes the transfer.  Two client classes exist, and both are stated by
the protocol:

- a *driver* client (HID, the console, the V9958 character path) is
  dispatched by the nucleus, so it may hold descriptors and may use
  `move` for same-domain staging (a driver-to-driver call is near-direct:
  the frame travels by reference and no bank switch happens).  This is
  the resident-domain convention;
- a *bank-7* client (the IOC class shim the module loader publishes to
  the IOC-FAT provider, `kernel/module/ioc_shim.c`, and the nucleus's own
  boot probe) can hold neither descriptor nor `move`, so it stages bytes
  through the common copy gate in `ZOS_SYSCALL_STAGE_SIZE` chunks against
  the service-window address the reply names.

### 12.1 SIO serial (class `ZOS_CLASS_SERIAL`, 1)

SIO0/B - the same channel as the early console - as a resident service.
Polling only: the SIO interrupt mode stays disabled, so the serial
service owns no IRQ and the early console keeps working before, and
without, any service.

| op | number | frame | result |
|----|--------|-------|--------|
| `ZSIO_OP_PUTC`  | 0 | `arg0` = byte | 0 or `-ZDEV_ETIMEDOUT` |
| `ZSIO_OP_WRITE` | 1 | desc IN, `arg0` offset, `arg1` length | bytes written |
| `ZSIO_OP_POLL`  | 2 | - | 1 when a byte is pending, else 0 |
| `ZSIO_OP_GETC`  | 3 | - | byte 0..255 or `-ZOS_EAGAIN` |
| `ZSIO_OP_READ`  | 4 | desc OUT, `arg0` offset, `arg1` length | bytes drained |

### 12.2 V9958 video (class `ZOS_CLASS_VIDEO`, 2)

The seven native VIDEO operations of `include/zephyros/video.h` plus the
character path the console composes.  The display reset
(`ZVIDEO_RESET`) is a hot-segment path: it re-runs the full bring-up,
including the font and atlas upload, reading the font from the image's
own bulk segment through `bulk_read` (the boot/bulk tier is not mapped in
the mode-11 service window).

| op | number | frame | result |
|----|--------|-------|--------|
| `ZVIDEO_OP_SEND`  | 0 | `arg0` = native VIDEO op; desc IN with `arg0` offset / `arg1` length for the five streaming ops | 0 or negative |
| `ZVIDEO_OP_WRITE` | 1 | desc IN, `arg0` offset, `arg1` length: bytes for the terminal parser | bytes consumed |
| `ZVIDEO_OP_PUTC`  | 2 | `arg0` = byte | 0 |

### 12.3 HID input (class `ZOS_CLASS_HID`, 4)

The M8 engine unchanged: fetch-on-doorbell with a 16-byte local ring, the
stuck-doorbell penalty, and strict reply validation before a byte is
committed.  The image declares its IOC dependency symbolically; the host
packer resolves it to the endpoint number and a missing IOC image is a
build error, so the driver never looks a peer up at run time.
`ZHID_OP_READY` is the run-time probe the console composes its input from
(the role the device registry's registration presence played before M10).

| op | number | frame | result |
|----|--------|-------|--------|
| `ZHID_OP_READ_TEXT` | 0 | desc OUT, `arg0` offset, `arg1` maximum | bytes transferred (0 when empty) |
| `ZHID_OP_POLL`      | 1 | - | 1 when a byte is buffered/just delivered |
| `ZHID_OP_GETC`      | 2 | - | byte or `-ZOS_EAGAIN` |
| `ZHID_OP_READY`     | 3 | - | 1 when the IOC dependency is up |

### 12.4 IOC transport (class `ZOS_CLASS_IOC`, 5)

The command/reply lane and the bulk lanes of the Z80-to-MCU transport.
The service owns wire framing, CRC, sequence, lane setup and the
READY/DONE bulk admission (`drivers/ioc/`); a client sees native
`struct zioc_frame` records (TYPE, STATUS, LENGTH, DATA[26]).

| op | number | frame | result |
|----|--------|-------|--------|
| `ZIOC_OP_COMMAND`    | 0 | `arg0` request type, `arg1` request DATA length (0..24), `payload[0..arg1-1]` request DATA | 0; reply `arg0` type, `arg1` DATA length, `payload[0..1]` staged reply-frame address, `payload[2]` reply STATUS, `payload[3..4]` bulk buffer address |
| `ZIOC_OP_REPLY`      | 1 | desc OUT, `arg0` offset, `arg1` length | bytes copied from the stored reply frame |
| `ZIOC_OP_BULK_READ`  | 2 | `arg1` length (1..512), optional desc OUT with `arg0` offset | length; without a descriptor `payload[0..1]` names the service buffer |
| `ZIOC_OP_BULK_WRITE` | 3 | `arg1` length (1..512), optional desc IN with `arg0` offset | length |
| `ZIOC_OP_STATUS`     | 4 | - | semantic status word (`ZIOC_STATUS_*`) |
| `ZIOC_OP_READY`      | 5 | - | 1 when the link is up and no recovery is pending |

The 29-byte native request frame does not fit the 24-byte inline payload,
and no deployed request class exceeds 24 DATA bytes, so the exchange
control travels in the frame words and `payload[3..4]` reports where the
bulk buffer is: a descriptor-less (bank-7) client writes the bytes it
wants written to that address through the copy gate and then issues
`ZIOC_OP_BULK_WRITE` without a descriptor.  `kernel/module/ioc_shim.c`
does exactly that for the IOC-FAT provider and disappears with it in
package 42.

### 12.5 Console (class `ZOS_CLASS_CONSOLE`, 6)

The console composes the drivers the build selected - HID input when its
resolved dependency is present and `ZHID_OP_READY` answers 1, otherwise
the serial driver; V9958 output when the video dependency is present,
otherwise the serial driver - exactly as `console_bind_defaults()` did for
the M8 device set.  The host packer resolves the three symbolic
dependencies (HID, SERIAL, VIDEO); an absent optional dependency resolves
to endpoint number 0, which is the fallback.  It is the endpoint the SDK
console functions and the monitor use; the early console and the panic
path do not depend on it.

| op | number | frame | result |
|----|--------|-------|--------|
| `ZCONSOLE_OP_WRITE` | 0 | desc IN, `arg0` offset, `arg1` length | bytes written |
| `ZCONSOLE_OP_READ`  | 1 | desc OUT, `arg0` offset, `arg1` length | bytes drained (non-blocking) |
| `ZCONSOLE_OP_GETC`  | 2 | - | byte or `-ZOS_EAGAIN` |
| `ZCONSOLE_OP_POLL`  | 3 | - | 1 when input is pending |
| `ZCONSOLE_OP_PUTC`  | 4 | `arg0` = byte | 1 or negative |

`ZCONSOLE_OP_PUTC` is the descriptor-less form: `zos_putc()` uses it (one
call, no copy), and the nucleus uses it for the boot transcript it prints
through the console service.  A store that lists no console service makes
the SDK console functions fail with `-ZOS_ENOENT` and leaves the boot
transcript on the early console.

### 12.6 Placement, IRQs and ledgers

The five images own bank 6's two resident tiers (architecture 36.6,
36.20): the host packer links each segment at its final address, and the
hot segments (`_CODE` + `_SERVICE_IO`, the one bank-6 range the mode-11
window and the mode-10 aperture both show) total 12,930 bytes of the
32,768-byte hot tier; the boot segments plus the V9958 font bulk segment
total 2,170 bytes of the 24,576-byte boot/bulk tier.  No driver image owns
an IRQ in this package: SIO0/B is polled, the IOC and HID paths are polled
through the same lane, and the V9958 is register-driven.  The arena holds
the IOC-FAT provider alone (13 granules), and the ledger lines the build
prints are
`zmodpack: driver hot: ...` / `driver boot/bulk: ...` with one row per
image (see `docs/memory.md`).

### 12.7 The `/dev` publication table (package 42a)

Programs never name an endpoint.  A driver's *publication* is a namespace
name under `/dev` plus the 8-bit protocol type of the instance that serves
it, and the whole mapping is built on the host (architecture 36.13,
decision 13):

1. A service manifest may carry `publishes` rows - the name *below* the
   `/dev` root (`"/serial/0"`), the protocol type, the serving endpoint as
   a (class, instance) pair, and the provider's own object identity.  The
   packer resolves the endpoint against the build's load order, strips the
   `/dev` root and nothing else, and refuses a row that names the root
   itself, a name that does not fit the 16-byte record, a duplicate name,
   an unknown class, or more rows than the nucleus table holds (12).
   Grouping rows are derived, never published: the packer counts them
   when it checks the table size (`/input` for `/input/keyboard`), and the
   provider synthesizes them from the names it installs.
2. The resolved rows become the container's publication table: ABI 4
   (`docs/module-format.md` section 7.8), 20 bytes per row, ordered after
   the segment, endpoint and dependency tables in the service section.
3. The loader validates each row (name shape, protocol type in range,
   endpoint number non-zero) and installs it in the DEVICE provider's
   table.  The provider synthesizes the grouping rows from the names it
   received - `/dev/serial/0` implies the container `/dev/serial` - so the
   table on the target always matches the packed store.

From a program the model is two calls (`docs/abi.md` section 12.1):

```c
unsigned char type;
int handle = zos_ipc_bind("/dev/serial/0", 0, &type);   /* type = SERIAL */
zos_ipc_call(handle, op, &req);                          /* one frame     */
zos_ipc_unbind(handle);
```

`bind` returns the handle *and* the type the publication declares: a
client that cares checks it, and there is no expected-type argument and no
`handle_info`.  A grouping name (`/dev/input`) enumerates but refuses to
bind with `-ZOS_EISDIR`.  Names are resolved case-insensitively, and the
lookup is a table scan in the provider - the host has already done every
decision that could be taken at build time.

The production build's five manifests still carry service ABI 3 (no
publications) and keep the stage-3 lookup path: the DEVICE provider, the
table and the client API are linked by the `namespace-selftest` acceptance
image (`make ns-selftest`), where the SIO and console manifests declare
`/dev/serial/0` and `/dev/console` and a synthetic publisher adds
`/dev/input/keyboard` and `/dev/video`.  Package 42b turns the production
manifests over and deletes the lookup path
(`docs/validation/2026-10-09-m10-namespace-handles.md`).
