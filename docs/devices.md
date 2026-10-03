# ZephyrOS Device, Driver, and Console Model (through M8 ABI v2)

This document describes the implemented device layer: the fixed device
registry, the driver ABI (v2 since M8), the SIO0/B driver (a dynamically
loaded module since M4), the IOC command/status driver (a dynamically
loaded module since M8), and the console service.  The architectural
contract is `docs/architecture.md` sections 20-22 and 30.2-30.3; this
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
kernel.  M8 (work package 15) adds the IOC command lane: a second
production ZMOD module loaded after SIO that speaks the deployed MCU
transport without any firmware change.  The M3 test-only static drivers
remain retired.

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
(`kernel/device/registry.c`, `include/zephyros/device.h`):

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
[platform_ctc0_test_start() in the M4 IRQ acceptance image]
boot module source (kernel_boot_module()):
    pre-M5 images -> module_catalog_load_all()
                     (embedded SIO module registers sio0.b)
    ROMFS images  -> romfs_init()
                     romfs_load_zmod("sio", "sio.zmod")
                     romfs_load_zmod("ioc", "ioc.zmod")   ; M8
                     (failure: "ioc: unavailable", SIO untouched)
[platform_ctc0_stop()]
console_bind_defaults()       ; capability lookup
dynamic console line via console service
[TEST-ONLY HOOK: zos_target_test_late() in acceptance builds]
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

## 5. IOC command/status driver (M8)

`drivers/ioc/` (`ioc.c`, `ioc_io.asm`, `ioc_module.c`, `ioc_driver.h`,
`ioc_proto.h`; manifest `modules/manifests/ioc.json`) is the second
production ZMOD module.  It implements the deployed Zephyr-80 host
transport contract (level 08h) over SIO1/B, the MCU command/reply lane,
without any IOC firmware change, and registers `ioc.0` only after its
bounded link bring-up produced a CRC-verified matching reply.

- Class and capabilities: `ZDEV_IOC` with
  `ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS`.  `bulk_read`/`bulk_write` are
  null and their capability bits are absent; the bulk lane arrives with
  the next M8 package.  The architecture's operation order
  (`command`, `bulk_read`, `bulk_write`, `status`) is pinned by
  `include/zephyros/ioc.h`.
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
  (`EBADFRAME`, `EBADCRC`, `EBADSEQ`, `ETIMEDOUT`, `EBUSY`, `EINVAL`).
- SIO1/B programming happens once, in the module init: channel reset,
  `WR1 = 00h` (no interrupts/WAIT), External Sync x1 (`WR4 = 30h`),
  `FFh` underrun fill (`WR7`), receiver disabled (`WR3 = C0h`), TX
  enabled with RTS inactive (`WR5 = 68h`).  A channel reset, receiver
  disable, or Enter Hunt destroys character synchronisation, so an
  ordinary transaction never issues one.
- Link bring-up: up to four `CMD_LINK_SYNC` attempts; each attempt
  re-arms local Hunt (the MCU clears its established flag per request),
  and `ioc.0` registers only when the reply is CRC-verified and is
  `RSP_LINK_SYNC`.  A failure returns `ZDRIVER_FAIL`; the loader rolls
  back, no device is published, and the SIO fallback is unaffected.
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
  DATA, and leaves the receiver enabled.  Every wait is bounded by the
  verified platform budgets.
- Serialization: a module-private foreground busy guard makes
  recursive/overlapping use fail with `ZDEV_EBUSY`; the M2 registry
  lock is never held over transport I/O.
- Boot integration: production images carry `sio.zmod`, `ioc.zmod`, and
  `monitor.zex` in the ROMFS recovery store and load SIO first, then
  IOC, as part of the deterministic boot set reported by the arena
  plan.  A failed IOC load/bring-up prints `ioc: unavailable` and the
  console still binds to SIO.  Pre-M8 acceptance images compile the IOC
  load out (`ZOS_IOC_BOOT=0`) and stay byte-identical to their accepted
  builds.
- Host validation: `tests/test_ioc.py` pins the platform constants,
  class contract, module artifact, boot order, and mapcheck rules;
  `tests/test_ioc_sim.py` replays the real M8 acceptance image against
  the simulator's IOC command-lane peer (exact wire bytes, one-time
  Hunt, fault injection, bounded failure, retry/recovery, IFF
  restoration, rollback, and semantic status).  The physical acceptance
  image is `build-m8-ioc-command-selftest/` and is driven by
  `make smoke-hardware-m8-ioc-command`; board timing remains a physical
  hardware authority.

## 6. Console service

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

## 7. Drivers and interrupts

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

## 8. Placement and validation

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
rejection sweep fails the build on any linked `_sio_`, `_sio0b_`, or
`_sio_driver` symbol (the SIO driver may exist only as a module), and
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
transport remain physical-hardware authorities; the simulations cover
control flow and register-level behavior.

## 9. Out of scope

No heap, no module unload or hot-plug, no filesystem API for modules
(beyond the two bounded DATA resource imports), no process API here, and
no user-visible device syscall.  M4 stages 07-08 (this document plus
`docs/module-format.md`) delivered the loader and the embedded
relocatable SIO driver; M8 work package 14 delivered driver ABI v2, the
read-only discovery calls, the per-class capability ranges, the
preferred-direction console policy, and the derived production arena;
M8 work package 15 delivered the IOC command/status class and its
deterministic boot load after SIO.  The M3 numeric registry model still
holds; its placeholder class names were refined in place.
