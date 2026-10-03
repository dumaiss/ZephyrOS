# ZephyrOS Device, Driver, and Console Model (M3, implemented)

This document describes the implemented M3 device layer: the fixed device
registry, the driver contract and static driver table, the SIO0/B driver,
and the console service.  The architectural contract is
`docs/architecture.md` sections 20-21; this file records what the build
actually places where and how it is validated.

M3 keeps every driver statically linked into the kernel image.  M4 will
replace the *source of driver code* (relocatable modules loaded into the
bank-7 driver arena); the registry, capability, and driver-ABI model below
does not change.

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
          └── ops:   putc/write/poll/getc (polling)
```

A capability may be provided by different devices in the future (a
V9958 provides `VIDEO` + `CONSOLE_OUT`; an HID device provides `INPUT` +
`CONSOLE_IN`), so console input and output bind independently.

## 2. Device registry

Fixed-size, heap-free, bank-7 kernel data
(`kernel/device/registry.c`, `include/zephyros/device.h`):

- 16 entries; handles are the slot index + 1, issued in the range
  `1..16`.  Handle `0` is invalid.  Handles are valid for one boot and are
  not an external ABI.
- Each entry stores copied metadata (class, capabilities, name pointer)
  and the driver-owned `ops`/`state` pointers; the registry never absorbs
  driver-private state.
- Registration rejects null descriptors, null/empty names, null ops,
  out-of-range classes, duplicate instance names (`ZDEV_EBUSY`), and
  registry overflow (`ZDEV_ENOSPC`) without touching existing entries.
- Membership lookup is by capability:
  `device_find_capability(required_caps, after)` returns the first
  registered device whose capability mask contains every required bit,
  starting after `after`; `0` means "no such device".
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

## 3. Driver contract and static table

`include/zephyros/driver.h`:

```c
#define ZDRIVER_ABI_VERSION 1u

struct zdriver_api {
        int (*device_register)(const struct zdevice_desc *, zdev_handle_t *);
        int (*irq_claim)(irq_source_t, irq_handler_t, void *);
        int (*irq_release)(irq_source_t);
};

struct zdriver {
        const char *name;
        uint8_t abi_version;
        int (*init)(const struct zdriver_api *api);
};
```

- Init convention: `ZDRIVER_OK` (0) or `ZDRIVER_FAIL` (-1).
- The explicit null-terminated `boot_drivers[]` table lives in
  `kernel/device/driver.c`; there is no linker-section discovery.
- `driver_init_static()` rejects drivers whose `abi_version` does not
  match `ZDRIVER_ABI_VERSION` (their init never runs), runs each accepted
  init through the checkpoint/rollback transaction, and returns the number
  of failed drivers.
- Drivers are not offered any other kernel service at M3.  In particular
  they never touch `I`, the IM2 page, the memory mode, the IRQ stacks, or
  `RETI`; those belong to the M2 executive.

Boot order in `kernel_main()` (after the M1/M2 stages):

```text
irq_init()
banner (early console)
[M1/M2 self-tests in test builds]
device_registry_init()
driver_init_static()          ; each driver registers its device(s)
console_bind_defaults()       ; capability lookup
console line via console service
[M3 self-test in test builds]
halt loop
```

## 4. SIO0/B static driver

`drivers/sio/` (`sio_io.asm`, `sio.c`, `driver.c`, `sio.h`):

- Ports: data `22h`, control `23h`; 115200 8N1, polled, interrupts stay
  disabled (`WR1 = 00h`).
- Registers exactly one device named `sio0.b`, class `ZDEV_SERIAL`, with
  `ZCAP_CHAR_IN | ZCAP_CHAR_OUT | ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT`.
- Init explicitly re-establishes the documented programming sequence
  (`18h, WR4=44h, WR3=E1h, WR5=E8h, WR1=00h`); it is idempotent with the
  early console's setup.
- Operations are bounded polling:
  - `putc` waits for TX-empty + CTS with a finite budget
    (`ZOS_SIO_TX_TIMEOUT`) and returns `ZDEV_ETIMEDOUT` on expiry;
  - `write` returns the number of bytes written (partial count on a
    timeout after the first byte);
  - `poll` returns 1/0 for RX-available;
  - `getc` returns the byte, or `ZDEV_EAGAIN` when nothing is pending.
- Driver-private state (`sio0b_state`, bank-7 data) tracks timeout/read
  counters; the registry only holds an opaque pointer to it.

The early console (`kernel/early_console.c`, common memory) remains a
completely separate, direct access path used by boot and panic.  Panic
output never depends on the registry, the driver, or the console service.

## 5. Console service

`kernel/device/console.c`, `include/zephyros/console.h`:

- Stores separate input and output handles.
- `console_bind_defaults()` finds the first device advertising
  `ZCAP_CONSOLE_OUT` and the first with `ZCAP_CONSOLE_IN`; it never
  compares driver or instance names.  A missing direction clears the
  binding and returns `ZDEV_ENODEV`.
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

Normal boot prints its acceptance line through this service:

```text
console: sio0.b OK
```

### 5.1 Fallback build options

Two compile-time options prove that the early diagnostic path is
independent of the device layer:

```sh
make m3-fallback      # builds both variant images
```

| Option | Behavior | Early-console diagnostic |
| --- | --- | --- |
| `ZOS_M3_NO_STATIC_DRIVERS=1` | skips `driver_init_static()` and binding | `console: static drivers disabled` |
| `ZOS_M3_NO_CONSOLE_BIND=1` | runs drivers, skips binding | `console: binding disabled` |
| runtime failure | binding finds no suitable device | `console: unbound` |
| runtime service failure | bound device reports an error | `console: service failed` |

The fallback images keep the full M0 banner and panic path.

## 6. Drivers and interrupts

A static driver may claim an M2 logical source through
`zdriver_api.irq_claim` and receives a bank-7 handler; the executive maps
bank 7 before dispatch and owns the final `RETI`.  The production SIO
driver stays polling-only.  The test-only probe driver
(`drivers/test/irq_probe.c`, `ZOS_M3_SELFTEST` builds) proves the path on
hardware: it claims `IRQ_CTC0` during init, a bounded EI spin receives
repeated deliveries, the handler records mode-11 mapping and bank-7 ISR
stack placement, then the source is released and CTC0 quiesced.

## 7. Placement and validation

All M3 driver/service code, descriptors, ops tables, and private state
live in bank 7 (`_CODE`/`_DATA`, below the `DF00h` ISR stack); none of it
may enter permanent common memory.  `tools/mapcheck.py` enforces this
with an explicit symbol list and a prefix sweep over `_device_`,
`_console_`, `_sio0b_`, `_sio_`, `_driver_`, `_boot_drivers`, `_m3_`
symbols.

Host validation layers:

- `tests/test_m3.py` – constant contract (classes, capabilities, negated
  errno statuses, driver ABI version), mapcheck bank-7 rules, fallback
  image artifacts, and static linker-file/object consistency.
- `tests/test_m3_sim.py` – replays the real linked M3 code in the Z80
  simulator: boot C-runtime init, registry/driver/console sequence, the
  full target self-test (registry semantics, rollback, exhaustion, ABI
  rejection, console TX/RX through the SIO ports, IRQ probe through the
  M2 executive with injected CTC0 vectors), and a production `kernel_main`
  boot replay ending at `console: sio0.b OK`.
- `make m3-selftest` – the physical acceptance image (banner + syscall +
  IRQ + M3 self-tests + interactive console RX).
- `make smoke-hardware-m3 SERIAL_PORT=...` – driving the acceptance image:
  captures the transcript and sends the `R` byte when the kernel announces
  `m3 selftest: ready for RX`.

SIO0/B timings, CTC0 delivery, and console RX remain physical-hardware
authorities; the simulations cover control flow and register-level
behavior.

## 8. Out of scope at M3

No heap, no dynamic modules/unload, no hot-plug, no filesystem, no process
API, and no user-visible device syscall.  M4 adds the module loader and
the embedded relocatable SIO driver on top of this unchanged model.
