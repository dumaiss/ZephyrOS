# ZephyrOS Device, Driver, and Console Model (through M8 ABI v2)

This document describes the implemented device layer: the fixed device
registry, the driver ABI (v2 since M8), the SIO0/B driver (a dynamically
loaded module since M4), the IOC command+bulk transport driver and the
HID keyboard/text driver (dynamically loaded modules since M8), and the
console service.  The architectural contract is `docs/architecture.md`
sections 20-22 and 30.2-30.3; this file records what the build actually
places where and how it is validated.

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
`hid.keyboard0`.  The M3 test-only static drivers remain retired.

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
values are stable: SERIAL=1, VIDEO=2, STORAGE=3, HID=4 (formerly INPUT),
IOC=5 (formerly TRANSPORT), OTHER=6; capability ranges are documented per
class (generic 0-3, VIDEO 8-14, HID 16-18, IOC 24-27).

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
[TEST-ONLY HOOK: zos_target_test_irq_hold_begin() in the M4 IRQ
 acceptance image only -- the concrete CTC0 programming is test-owned
 (tests/target/support/ctc_test.c)]
boot module source (kernel_boot_module()):
    pre-M5 images -> module_catalog_load_all()
                     (embedded SIO module registers sio0.b)
    ROMFS images  -> romfs_init()
                     romfs_load_zmod("sio", "sio.zmod")
                     romfs_load_zmod("ioc", "ioc.zmod")    ; M8
                     romfs_load_zmod("hid", "hid.zmod")    ; M8
                     romfs_load_zmod("v9958", "v9958.zmod") ; M8
                     (failure: "<name>: unavailable 0x<ZOS_E*>"
                      on the early console -- the exact code names a
                      missing entry 0x05, a malformed container 0x0A,
                      an exhausted arena 0x06 or a refused init 0x0B;
                      the SIO fallback and every earlier resident
                      module are untouched)
[TEST-ONLY HOOK: zos_target_test_irq_hold_end()]
console_bind_defaults()       ; one final call, after all four loads
dynamic console line via console service
[TEST-ONLY HOOK: zos_target_test_late() in acceptance builds]
monitor launcher (unchanged process_exec("monitor.zex"))
halt loop
```

Acceptance code is selected only under `tests/target/support/`; the two
hooks above are the entire production-side integration surface.

## 4. SIO0/B module driver

`drivers/sio/` (`sio_io.asm`, `sio.c`, `sio_module.c`, `sio.h`, plus the
production manifest `modules/manifests/sio.json` and the shared
module-runtime glue `modules/runtime/sdcc_indirect_calls.asm`):

- The driver code is built, packed, and embedded as a ZMOD module; no SIO
  symbol is linked into the kernel image (enforced by `mapcheck.py`).
  Module sources must avoid byte-fragmented relocations, so the device
  descriptor is a `static const` and the module provides its own
  `___sdcc_call_iy/hl` glue (`docs/module-format.md` section 6).
  `sio_module.c` is production-only; the IRQ self-test and init-failure
  entries live under `tests/modules/` (`sio_irq_module.c`,
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
  (`build-m8-fb-nohid`, `-nohid-nov9958`, `-badioc`, `-arena`, plus
  the font-failure and HID rollback images) boot the same four-module
  sequence with one manifest/interface failure and are replayed
  against their exact bindings by `tests/test_m8_fallback.py` (see
  section 8.2).  Pre-M8
  acceptance images compile the IOC and HID loads out (`ZOS_IOC_BOOT=0`
  / `ZOS_HID_BOOT=0`) and stay byte-identical to their accepted builds.
- Host validation: `tests/test_ioc.py` pins the platform constants of
  both lanes (including the explicitly paired bulk CRC bypass),
  class contract, module artifact, boot order, and mapcheck rules;
  `tests/test_ioc_sim.py` replays the real M8 acceptance image against
  the simulator's IOC command+bulk peer (exact wire bytes on both
  lanes, READY/DONE interlocks, one-time Hunt per lane, fault injection
  on both directions, bounded failure, whole-link recovery, canaries,
  IFF restoration, rollback, and semantic status).  The physical
  acceptance image is `build-m8-ioc-selftest/` and is driven by
  `make smoke-hardware-m8-ioc`; board timing remains a physical
  hardware authority.  The target diagnostic runs the full /SHARED
  READY/DONE acceptance (write 1/128/512, matching DONE records, read
  back and verify, delete the dedicated `M8IOCTST.BIN`, prove the delete
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
  `build-m8-fb-badioc` malformed-IOC and `build-m8-fb-nohid`
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
  image is `build-m8-hid-selftest/`, driven by
  `make smoke-hardware-m8-hid`: the USB-keyboard pass is typed by the
  operator, the tool validates the echoed `m8 hid: rx XX` transcript,
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
  disabled: `IRQ_VDP_FF` is never claimed in M8.
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
  and the `build-m8-fb-arena` allocation-exhaustion image cover both
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
  `build-m8-v9958-selftest/`, driven by `make smoke-hardware-m8-v9958`;
  the console image `build-m8-console-selftest/` (HID input + V9958
  output) is driven by `make smoke-hardware-m8-console` and by
  `tests/test_m8_sim.py`; the operator confirms the documented pattern
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
  binding-independent path used by the boot banner, the M8 target
  control transcripts and `panic()`.  It never consults the console
  service, so it works even when the bound pair is HID/V9958.

The console service composes one bound input with one bound output;
neither module knows about the other.  The acceptance image
`build-m8-console-selftest/` binds HID input to V9958 output and runs the
unchanged M7 `monitor.zex` through that pair; `make
smoke-hardware-m8-console` validates the early-SIO boot transcript while
the operator confirms the visible session.  The package-20 integration
image `build-m8-selftest/` binds the same pair, runs the package-20
target diagnostic, and is driven by `make smoke-hardware-m8` followed by
the human monitor pass (section 8.2).

### 6.1 Failure-path evidence

The retired M3 fallback flags (`ZOS_M3_NO_STATIC_DRIVERS`,
`ZOS_M3_NO_CONSOLE_BIND`) no longer exist.  The M4 acceptance matrix is
the fallback evidence:

| Scenario | Behavior | Early-console diagnostic |
| --- | --- | --- |
| runtime failure | module load/init reports failures (`make m4-failure-selftest`, `tests/modules/sio_fail_module.c`) | `console: dynamic driver failed` |
| runtime failure | binding finds no suitable device | `console: unbound` |
| runtime service failure | bound device reports an error | `console: service failed` |

Every production image keeps the full M0 banner and panic path.

### 8.2 M8 integration acceptance (package 20)

Package 20 freezes the M8 production device stack and adds its
integration gate:

- **Boot contract**: `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod`
  (plus `v9958-6x8.font` and `monitor.zex`) are the only production
  ROMFS entries and are loaded in that order; a single
  `console_bind_defaults()` runs after all four loads, followed by the
  unchanged `process_exec("monitor.zex")` launcher.  A failed optional
  module prints `<name>: unavailable 0x<ZOS_E*>` on the direct early
  SIO (0x05 missing entry, 0x06 arena exhausted, 0x0A malformed
  container, 0x0B refused init) and the boot continues; ROMFS
  corruption keeps the fatal recovery policy.
- **Failure/fallback matrix** (each row asserts its exact binding,
  diagnostics and observable direction from the real linked bytes):

  | Image | Injected failure | Early-SIO diagnostic | Binding |
  | --- | --- | --- | --- |
  | `build-m8-selftest/`, `build/` | none (all four modules) | -- | HID/V9958 |
  | `build-m8-v9958-fontfail-selftest/` | undersized ROMFS font | `v9958: unavailable 0x0B` | HID/SIO |
  | `build-m8-fb-nohid/` | `hid.zmod` entry absent | `hid: unavailable 0x05` | SIO/V9958 |
  | `build-m8-fb-badioc/` | fake IOC (null command op) fails HID init | `hid: unavailable 0x0B` | SIO/V9958 |
  | `build-m8-fb-nohid-nov9958/` | HID and V9958 entries absent | both `unavailable 0x05` | SIO/SIO |
  | `build-m8-fb-arena/` | `v9958.zmod` replaced by an oversized allocation | `v9958: unavailable 0x06` | HID/SIO |

- **Host/simulator evidence**: `make m8-selftest` builds the
  integration image (full stack + package-20 target diagnostic + monitor
  launcher) and replays the production bytes
  (`tests/test_m8_acceptance.py`): boot order and binding, IOC
  command/status/repeated-PING and the /SHARED READY/DONE bulk
  interlock, HID queue traffic with the stuck-doorbell penalty, the
  scripted monitor session with two Q exit/relaunch cycles,
  malformed/null/class/capability rejections with device-table,
  granule-allocation and lane-handshake invariants, and post-fault
  recovery.  `make m8-fallback-selftests` builds every variant above
  and replays `tests/test_m8_fallback.py`; `tests/test_m8.py`
  reconciles the aligned linked-kernel end, derived arena, arena plan
  and image manifest, and checks every M8 ROMFS manifest's
  content/types/order.
- **Physical gate**: flash `build-m8-selftest/zephyros-m8-selftest.bin`,
  run `make smoke-hardware-m8` before reset, perform the human
  HID/V9958 monitor pass, then run each `make smoke-hardware-m8-fb-*`
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
module stays polling-only.  Two test paths prove the IRQ integration:

- the M3 target test (`tests/target/m3/m3_irq_probe.c`) claims
  `IRQ_CTC0` directly, a bounded EI spin receives repeated deliveries,
  the handler records mode-11 mapping and bank-7 ISR stack placement,
  then the source is released and CTC0 quiesced;
- the SIO IRQ self-test module variant
  (`tests/modules/sio_irq_module.c`, `tests/modules/sio-irq.json`,
  `make m4-selftest`) claims `IRQ_CTC0` from its *relocated* init while
  the kernel keeps CTC0 running with the M2 lock held (pre-claim ticks
  stay pending — an unowned dispatch would quiesce the timer), counts
  three deliveries inside a bounded EI spin, verifies its relocated
  state pointer, and releases the source before registering the device.

The CTC0 programming these acceptance paths need is test-owned
(`tests/target/include/ctc_test.h`, `tests/target/support/ctc_test.c`)
and is linked only into the M2/M3/M4 acceptance images.  The production
CTC owner (`drivers/ctc/ctc.c`) keeps only `platform_irq_quiesce()`;
`platform_ctc0_test_start()`, `platform_ctc0_stop()`, and the obsolete
`platform_irq_vector()` are not declared or linked in production, and
the kernel's M4 boot phase reaches the test timer only through the
generic `zos_target_test_irq_hold_begin()`/`_end()` seam hooks.

## 10. Placement and validation

All driver/service code, descriptors, ops tables, and state live in
bank 7 (linked `_CODE`/`_DATA` for the remaining kernel objects; the
loaded module inside the M8 driver arena).  None of it may enter
permanent common memory.  Production derives the arena start from the
aligned linked-kernel end (at least 16 KiB before `C000h`); pre-M8
acceptance images keep the fixed `A000h-BFFFh` 16-granule geometry (see
`docs/memory.md` section 2.2 and `docs/module-format.md` section 1).
`tools/mapcheck.py` enforces this with an explicit symbol list and a
prefix sweep over `_device_`, `_console_`, and `_m3_` symbols (the M3
acceptance symbols remain bank-7-resident in test images).  A
rejection sweep fails the build on any linked `_sio_`, `_sio0b_`,
`_sio_driver`, `_ioc_`, `_hid_`, or `_v9958_` symbol (the SIO, IOC, HID
and V9958 drivers may exist only as modules), and
`mapcheck.py --production` additionally rejects every acceptance symbol
(static-driver remnants, test runners/results, fixture catalogs, test
SIO module entries) in the production image.

Host validation layers:

- `tests/test_m8.py` – driver ABI v2 structure/order and capability-range
  contract, derived-arena geometry (alignment, 16-KiB minimum, unaligned
  relation, linked bytes in the arena), the boot-set arena plan and its
  overflow rejection, ABI v1 rejection, and the architectural granule
  bound.
- `tests/test_m3.py`, `tests/test_m4.py` – constant contracts, mapcheck
  rules (including the static-SIO rejection and arena ownership),
  catalog embedding identity, placement-base shift, fallback image
  artifacts, and linker-file/object consistency.
- `tests/test_m3_sim.py` – replays the real linked M3 code in the Z80
  simulator: boot C-runtime init, registry/driver/module-load/console
  sequence, the full target self-test (registry semantics, rollback,
  exhaustion, ABI rejection, console TX/RX through the SIO ports, IRQ
  probe through the M2 executive with injected CTC0 vectors), and a
  production `kernel_main` boot replay ending at
  `console: dynamic sio0.b OK`.
- `tests/test_m4_sim.py` – replays the loader and the dynamic-driver
  acceptance images (module catalog load with injected CTC0 deliveries,
  M8 ABI v2 fixture, ABI v1 rejection, console binding policy, failure-
  image rollback, shifted placement) in the same simulator.
- `tests/test_m6_sim.py` – replays the M6 acceptance image, including
  the driver-API ROMFS resource-import checks (DATA-only, bounded,
  byte-identical to the internal reader).
- `tests/test_hid.py` / `tests/test_hid_sim.py` – the HID class/driver
  contract and the real SIO+IOC+HID image replay (section 6).
- `tests/test_v9958.py` / `tests/test_v9958_sim.py` – the V9958 VIDEO
  class/driver contract, the font asset pipeline, and the real
  SIO+IOC+HID+V9958 image replay against the V9958 peer (section 7).
- `make m3-selftest` / `make m4-selftest` / `make m4-placement-selftest`
  / `make m4-failure-selftest` – the physical acceptance images.
- `make smoke-hardware-m3 SERIAL_PORT=...` – driving the acceptance image:
  captures the transcript and sends the `R` byte when the kernel announces
  `m3 selftest: ready for RX`.
- `make smoke-hardware-m4 SERIAL_PORT=...` /
  `make smoke-hardware-m4-failure SERIAL_PORT=...` – capture the dynamic
  console line plus `m4 selftest: OK` (or the failure transcript plus
  `m4 failure selftest: OK`).

SIO0/B timings, CTC0 delivery, console RX, and the IOC command-lane
and bulk-lane transports remain physical-hardware authorities; the
simulations cover control flow and register-level behavior.  HID input
arrives from the deployed MCU translation; physical USB-keyboard
acceptance is typed by the operator into the smoke run.

## 11. Out of scope

No heap, no module unload or hot-plug, no filesystem API for modules
(beyond the two bounded DATA resource imports), no process API here, and
no user-visible device syscall.  M4 stages 07-08 (this document plus
`docs/module-format.md`) delivered the loader and the embedded
relocatable SIO driver; M8 work package 14 delivered driver ABI v2, the
read-only discovery calls, the per-class capability ranges, the
preferred-direction console policy, and the derived production arena;
M8 work packages 15-16 delivered the IOC command+bulk class and its
deterministic boot load after SIO; M8 work package 17 delivered the HID
keyboard/text class, its boot load after IOC, and the input-only
`hid.keyboard0` device; M8 work package 18 delivered the V9958 video
core: the `v9958.0` VIDEO device with the seven native operations, the
ROMFS-backed font atlases and the command/VRAM primitives, loaded after
HID (character output and the terminal remain with the console
package).  The M3 numeric registry model still holds; its placeholder
class names were refined in place.
