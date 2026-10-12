# ZephyrOS Device, Driver, and Console Model (through M10 drivers)

This document describes the implemented driver layer.  M10 replaced the
M4-M9 model - drivers as bank-7 ZMOD modules registered in a device
registry, with a console device and console syscalls on top - with
**resident drivers**: the SIO0/B serial, IOC command+bulk transport, HID
keyboard/text and V9958 video driver stacks each run in bank 6 as a
driver image, the console is a fifth driver that composes them, and the
SDK and the nucleus reach all of it through the versioned protocols in
`abi/zos/protocol.h` (section 12 below).

Package 42c deletes what M10 left behind: the device registry, the
device class/capability model and the driver ABI are **gone**, together
with the `SYS_CONSOLE_*` syscalls and the console device object.  A
driver image declares the namespace names it publishes and the peer
endpoints it depends on, the host packer resolves both against the build's
load order, and the nucleus installs the resulting tables - nothing
registers itself at run time and no driver looks a peer up.  A driver is
entered only through `struct driver_boot` (section 12), and the last
arena module, the IOC-FAT provider, became the FS driver image that
publishes `/fs`.  The architectural contract is `docs/architecture.md`
sections 36.4-36.14; the image format is `docs/module-format.md`.

Sections 1-11 below are the M8/M9 implementation record (the class and
capability vocabulary, registration semantics, per-driver bindings);
section 12 is the current contract.  Read the earlier sections for the
driver internals and hardware behavior they describe, not for the
interfaces.

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

M10 vocabulary (section 12 is the contract):

```text
driver image   one bank-6 driver image: segments, its endpoints and the
                namespace names it publishes (docs/module-format.md)
endpoint        a (class, instance) protocol entry with a host-assigned
                number; the way anything is reached
protocol class  SERIAL=1, VIDEO=2, BLOCK=3, INPUT=4, IOC=5, CONSOLE=6
                (abi/zos/object.h); the 8-bit type of a publication
publication     a name below /dev (or the FS root) bound to a serving
                endpoint: how a name is reached
dependency      a declared peer endpoint, resolved by the host packer to
                a number the image receives in its boot bundle
```

```text
SIO driver image (drivers/sio)
    │
    ├── endpoint  SERIAL/0, version 1
    │     putc, write, poll, getc, read (polling)
    └── publishes /dev/serial/0
```

Nothing is registered at run time: reachability is the host's endpoint
numbering plus the publication table the loader installs.  A client holds
an endpoint number and calls it; a driver that needs a peer declares a
dependency and reads the resolved number.  Unsupported operations fail
predictably with a negative `ZOS_E*`/class error rather than being
inferred from a name.

The M3-M9 vocabulary this replaced - a driver registering one or more
named **devices** in a fixed kernel registry, each carrying a class and a
capability mask, reached through generic `char_ops` or a class-specific
operations table, with a separate `SYS_CONSOLE_*` device API - is
**deleted** (M10, work package 42c).  Class values survive as the
protocol numbers above; capabilities, handles, registry slots and the
block class contract do not.  A capability-like preference (console input
from HID, output from V9958) is now the console driver's own composition
policy over resolved dependencies.

M9's filesystem provider `iocfat.zmod` is the FS driver image: it talks
to the IOC driver's protocol only (`FS2` commands `30h-44h`, responses
`B0h-C4h`), owns a one-slot write-through 512-byte file-data cache, and
publishes `/fs`; `drivers/ioc/` was never changed for it.  Its acceptance
image is `make fs-selftest` (see
[`docs/validation/2026-10-05-m9-filesystem.md`](./validation/2026-10-05-m9-filesystem.md)).

**One error means "`/fs` has no media": `ZOS_ENODEV` (`03h`).**  A card
that is absent, removed or unreadable is not a driver-init failure (a
required driver that cannot initialize is a boot failure, and `/system`
must stay usable): the provider publishes, reports the card state once
and then answers `(-ZOS_ENODEV)` to *every* operation - `bind`, `stat`,
`ns_enum`, `chdir` below `/fs` and the FILE protocol alike.  The card
state is decided by the one command that reads the medium, the root
directory open *and* its first read: the deployed firmware answers
`OPENDIR` for the root from its mount context without touching the card
and fails only at the first `READDIR` (`FR_NOT_READY` -> `FS2` `NO_MEDIA`
-> `ENODEV`), so a volume whose card was pulled opens a root and proves
nothing (work package 45b correction 01; the simulator's IOC model
answers the same pair).  `ZFS_OP_STAT` on the root proves it the same way
before answering "directory", because the driver otherwise answers the
root from its own knowledge.

Two endpoints may serve the same role (a V9958 video endpoint; an HID
input endpoint), so the console driver composes input and output
independently.

## 2. Device registry (deleted in M10)

> **Deleted (M10, work package 42c).**  The fixed-size, heap-free bank-7
> device registry, its handles, its registration transaction, its
> checkpoint/rollback pair and the IRQ claims stored beside it are gone,
> with `kernel/device/` and the module-facing device header.  Reachability
> is now the **endpoint table** (numbers the host assigned, installed by
> the loader before any image runs) and the **publication table** (names
> below `/dev` bound to those numbers); a client that needs a peer
> receives the peer's number in its boot bundle, so there is nothing to
> search, claim or roll back.

The M8/M9 record, kept because the driver sections below refer to it:

- 16 entries; handles were the slot index + 1, issued in the range
  `1..16`, with `0` invalid.  Handles were valid for one boot and were
  never an external ABI.
- Each entry stored copied metadata (class, 32-bit capability mask, name
  pointer) and the driver-owned `char_ops`/`class_ops`/`state` pointers;
  the registry never absorbed driver-private state.
- Registration rejected null descriptors, null/empty names, descriptors
  with neither a character nor a class interface, out-of-range classes,
  duplicate instance names and registry overflow, without touching
  existing entries.
- Discovery was read-only: a class id plus the required capability bits
  found the first matching live entry, and an interface call verified the
  live handle, the class and a non-null class interface; invalid handles,
  stale slots and class mismatches had their own errors.  No client could
  mutate the registry.
- Registry mutation ran with maskable interrupts disabled through the M2
  executive lock.

**Registration transaction (also deleted).**  Each driver initialization
ran inside a checkpoint/rollback pair: a checkpoint snapshotted the
registration generation, and a failed init removed exactly the entries
created after that snapshot (wrap-safe comparison), never those of an
earlier driver.  M10 needs no equivalent: an image's endpoints and
publications are data the host produced, and the loader either installs
them all before the image runs or refuses the image and reports a boot
error (architecture 36.12).

---

## 3. Driver contract (deleted in M10)

> **Deleted (M10, work package 42c).**  `formats/include/zmod.h`, the
> driver ABI version word, the kernel import block it carried
> (registration, discovery, IRQ claim/release and the two ROMFS resource
> imports) and every class header are gone.  A driver image has no
> import table at all: it receives the nucleus interface table and its
> resolved dependency numbers through `struct driver_boot`
> (section 12), and the only peer interaction it can make is a call to an
> endpoint number.

The M8/M9 record: the ABI block exposed a registration call, an IRQ
claim/release pair, two read-only discovery calls (class plus capability
mask, then interface lookup with a class check), and two bounded ROMFS
`DATA` resource imports that never handed out reader state.  Its callback
order and structure layouts were pinned by compile-time assertions and
host tests, and `driver.h` plus its class headers were the complete
module-facing compile-time surface (the registry lifecycle, the
IRQ-executive internals and the raw memory-latch/bank primitives stayed
kernel-private; `tests/test_source_layout.py` resolves the module-facing
include closure and compiles a representative module against it).  Init
convention was `0` on success or `-1` on failure; a driver never touched
the interrupt flag, the IM2 page, the memory mode, the IRQ stacks or
`RETI`.

The M4 loader accepted that ABI version in the container header and
passed the block to the module; the M3 static driver table (`boot_drivers
[]`, the static init entry, the `_driver_static_*` symbols) was retired
by M4 and `tools/mapcheck.py --production` still rejects any static-driver
or acceptance symbol that would creep back in.

**Boot order replaced.**  The M9 path - `device_registry_init()`, the
module-loader init, a VFS open per boot module with the loader placing
each one in the derived bank-7 arena, then a console binding call - is the
M9 record in section 5 below and no longer describes the build.  The
current path is architecture 36.12 (`vfs_init`, `driver_load_all`, the
selected console driver's init, monitor); the acceptance code is selected
under `tests/target/support/` and the two target hooks are the entire
production-side integration surface.

## 4. SIO0/B driver image

> The per-driver sections 4-7 describe the M8/M9 **driver internals** -
> port programming, protocols, failure policy - which still hold; the
> registration, class/capability and operations-table vocabulary in them
> is the M8 record, and the current contract is section 12.

`drivers/sio/` (`sio_io.asm`, `sio.c`, `sio_module.c`, `sio.h`, plus the
production manifest `modules/manifests/sio.json` and the shared
module-runtime glue `drivers/runtime/sdcc_indirect_calls.asm`):

- The driver code is built, packed, and loaded from the recovery store as
  a ZMOD module; no SIO symbol is linked into the kernel image (enforced
  by `mapcheck.py`).
  Module sources must avoid byte-fragmented relocations, so the device
  descriptor is a `static const` and the module provides its own
  `___sdcc_call_iy/hl` glue (the M4 container carried no call helpers;
  `docs/module-format.md` describes today's driver image).
  `sio_module.c` is production-only; the IRQ and init-failure container
  fixtures live under `tests/modules/` (`sio_irq_module.c`,
  `sio_fail_module.c`) with their own manifests.
- Ports: data `22h`, control `23h`; 115200 8N1, polled, interrupts stay
  disabled (`WR1 = 00h`).
- Registered exactly one `sio0.b` device in M8/M9; in M10 the image
  exports the SERIAL protocol endpoint instead.
- Init re-establishes the documented programming sequence
  (`18h, WR4=44h, WR3=C1h, WR5=EAh, WR1=00h`); it is idempotent with the
  early console's setup.
- Operations are bounded polling:
  - `putc` waits for TX-empty with a finite budget
    (`ZOS_SIO_TX_TIMEOUT`) and returns the timeout error on expiry;
  - `write` returns the number of bytes written (partial count on a
    timeout after the first byte);
  - `poll` returns 1/0 for RX-available;
  - `getc` returns the byte, or `ZOS_EAGAIN` when nothing is pending.
- Driver-private state lives in the module's zero-initialized allocation
  (relocated `_DATA`); the registry only holds the relocated pointer.

The early console (`kernel/early_console.c`, common memory) remains a
completely separate, direct access path used by boot and panic.  Panic
output never depends on the registry, the driver, or the console driver.

## 5. IOC command + bulk transport driver (M8)

`drivers/ioc/` (`ioc.c`, `ioc_io.asm`, `ioc_module.c`, `ioc_driver.h`,
`ioc_proto.h`; manifest `modules/manifests/ioc.json`) is the second
production ZMOD module.  It implements the deployed Zephyr-80 host
transport contract (level 08h) over both SIO lanes -- SIO1/B for
command/reply and SIO1/A for bulk DATA -- without any IOC firmware
change, and registers `ioc.0` only after its bounded link bring-up
produced a CRC-verified matching reply.

- Client contract: `drivers/include/ioc.h` is the class ABI;
  `drivers/include/ioc_protocol.h` is the client-visible driver
  vocabulary -- the deployed MCU command/reply classes and DATA payload
  offsets (PING, HID input fetch) a protocol-aware driver or target
  diagnostic may share.  `drivers/ioc/ioc_proto.h` is transport-private
  and extends that vocabulary with the READY/DONE bulk constants, the
  frozen bulk-arming class gate, and the controller status bytes; no
  production driver other than the IOC driver includes it.  A driver
  reaches its own directory's private headers through same-directory
  quote includes, never through a shared `-I drivers/<name>` module flag.
- Contract: the IOC protocol endpoint (class 5) with the command,
  bulk-read, bulk-write and status operations in that order;
  `ZIOC_BULK_MAX` is 512 DATA bytes.
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
  recursive/overlapping use fail with `ZOS_EBUSY`; no kernel lock is
  held over transport I/O.
- Boot integration: production images carry `sio.zmod`, `ioc.zmod`,
  `hid.zmod`, `v9958.zmod`, the selected console image and `iocfat.zmod`
  in the ROMFS recovery store, named by `drivers.man` in load order.
  A failed IOC load or init is a loader boot error with the loader's own
  line; a down link at init is not a load refusal (the first ordinary
  operation recovers it), and the serial console variant keeps working
  without the MCU link.  The M8-era per-image boot gates and the
  observable fallback images were retired with the runtime console
  fallback (work package 42e; see section 8.2).
- Host validation: `tests/test_ioc.py` pins the platform constants of
  both lanes (including the explicitly paired bulk CRC bypass),
  class contract, module artifact, boot order, and mapcheck rules;
  `tests/test_ioc_sim.py` replays the real IOC acceptance image against
  the simulator's IOC command+bulk peer (exact wire bytes on both
  lanes, READY/DONE interlocks, one-time Hunt per lane, fault injection
  on both directions, bounded failure, whole-link recovery, canaries,
  IFF restoration, rollback, and semantic status).  The transport runs
  in the console-integration image (`build-console-integration-selftest/`)
  since work package 42d merged the per-device images; its physical gate
  is `make smoke-hardware-console-integration`, and board timing remains a
  physical hardware authority.  The target diagnostic runs the full /SHARED
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
implements the native HID class (`drivers/include/ioc_protocol.h`) on top
of the
IOC transport and registers exactly one input-only device after it has
discovered the IOC class; the MCU firmware is untouched.

- Contract: the HID protocol endpoint (class 4); M8 implemented the text
  operation only.  Key-event and gamepad shapes were reserved and are
  absent; an unsupported operation must be reported as an error by the
  dispatcher, never inferred from a name.
- Dependency discovery became **host resolution**: the image declares a
  symbolic dependency on the IOC endpoint (M8: a class/capability search
  plus an interface lookup), the packer resolves it to a number, and the
  driver reads that number from its boot bundle.  The module contains no
  imported IOC symbol and no name lookup; its only IOC compile-time
  surface is the public `drivers/include/ioc_protocol.h` vocabulary,
  never the IOC driver's private headers.  A missing dependency is a pack
  error; an absent optional one resolves to 0 and the driver reports its
  own failure, after which the SIO fallback and the IOC driver stay
  untouched.
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
  it.  A transport failure is reported as no input (0 / `ZOS_EAGAIN`)
  with the penalty throttling retries, so a broken IOC link cannot wedge
  console polling.  HID never samples SIO1, RR0, CTS or any port; raw
  status stays an IOC implementation detail.
- Boot integration: production images load SIO, then IOC, then HID
  before the console driver; a missing or refused image is the loader's
  boot error (section 8.2).  A down IOC link at init keeps HID
  registered and reports it through `ZHID_OP_READY`; `console-v9958-hid`
  does not probe it (the console is build-selected, not composed), and
  the first ordinary fetch retries the whole-link recovery.  The M8
  `console_bind_defaults()` composition and its fallback images are gone
  (work package 42e).
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
  and the malformed-IOC interface rejection).  The HID diagnostic runs
  in the console-integration image (`build-console-integration-selftest/`)
  since work package 42d merged the per-device images, and its physical
  gate is `make smoke-hardware-console-integration`: the USB-keyboard pass
  is typed by the operator (the integration diagnostic waits for its
  `K41` line), and the tool validates the echoed transcripts.

## 7. V9958 video core (M8)

`drivers/v9958/` (`v9958.c`, `v9958_io.asm`, `v9958_module.c`,
`v9958_driver.h`, `assets/font_cp850_6x8.inc`; manifest
`modules/manifests/v9958.json`) is the fourth production ZMOD module:
the direct-hardware LunchCrema V9958 core ported from the pinned
Zephyr-80 reference.  It delivers the hardware-safe initialization, the
native VIDEO class operation surface, the ROMFS-backed font atlas, the
low-level graphics/command primitives, and (package 19) the ported 85x26
6x8 terminal renderer behind an output-only character interface, so the
same hardware serves graphics clients and the normal text console.

- Contract: the VIDEO protocol endpoint (class 2) with the seven native
  operations, plus the character path the console composes: its write
  operation feeds one byte or the whole 16-bit length into the terminal
  parser, and the read side is absent (output only).  The console driver
  therefore selects it as its output when the endpoint is present, while
  input comes from the HID endpoint.
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
- Private renderer stack: the character entries are assembly wrappers
  (`v9958_io.asm`) that switch to a private 256-byte module stack before
  entering the C workers, because a complete renderer call chain exceeds
  the common syscall gate stack depth that syscall-context callers run
  on.  The workers preserve the exact sdcccall(1) prototypes and return
  values; the gate stack itself is sized for the deepest accepted
  syscall chain (see `docs/memory.md`).
- Native operations (`drivers/include/ioc_protocol.h`):
  `ZVIDEO_CTRL_WRITE` (1), `ZVIDEO_DATA_WRITE` (2),
  `ZVIDEO_DATA_BLOCK` (3), `ZVIDEO_PALETTE_WRITE` (4),
  `ZVIDEO_INDIRECT_WRITE` (5), `ZVIDEO_PRESENT` (6), `ZVIDEO_RESET`
  (7), dispatched through the VIDEO endpoint's send operation.  The
  numbers are native,
  not Virtual Drip packet numbers.  `send` validates before any I/O:
  write operations require a non-null payload and a nonzero length and
  stream the full 16-bit length (native ZephyrOS does not inherit the
  historical 16-byte non-block ceiling); PRESENT/RESET require zero
  length; unknown operations return the unsupported-operation error,
  malformed requests `ZOS_EINVAL`.
- Hardware discipline (ports A0h-A4h; declarative ports, latch bits,
  register values and the bounded bootstrap delay live in
  `platform/zephyr80/public/hardware.{h,inc}`, paired and parity-tested): the
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
- Boot integration: production images carry `v9958.zmod` (its font
  travels in the image's bulk segment) in the ROMFS recovery store and
  load it before the console driver, which declares it as a required
  dependency.  A missing/undersized font resource or a failed VDP
  sequence is refused by the packer or fails the loader's init check;
  the M8 font-failure and allocation-exhaustion images were retired with
  the module format they exercised (section 8.2).
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
  `tests/test_v9958_console.py` drove the module's character interface
  (retired in M10: the character contract lives in the VIDEO protocol and
  is covered by `tests/test_v9958.py`) and
  `ZVIDEO_RESET` with scripted byte streams and decodes the VDP command
  trace into a physical-bitmap screen model (glyph copies, fills, row
  copies, R#23 origin); fixed golden cases cover printable placement,
  CR/LF/VT/BS/TAB/FF, right-margin wrap and DECAWM off, cursor
  boundaries and CSI families, erase display/line/characters, SGR
  reverse video and reverse-background fills, save/restore, insert/
  delete lines, circular scrolling, cursor visibility, all reset paths,
  print-run batching, arbitrary write chunking, and exact command-block
  geometry.  The V9958 diagnostic runs in the console-integration image
  (`build-console-integration-selftest/`, work package 42d) with the
  console sessions (HID input + V9958 output) the retired per-device
  images carried; it is driven by
  `make smoke-hardware-console-integration` and replayed by
  `tests/test_console_acceptance.py`, and the operator confirms the
  documented pattern and the visible monitor session.

## 8. Console drivers (build-selected)

`drivers/console/console_proto.c` (work package 42e; one shared
source compiled into two images):

- The console is a **driver image**, not kernel code: each image owns one
  endpoint (class `ZOS_CLASS_CONSOLE`, instance 0) and publishes
  `/dev/console`.  It is the endpoint the SDK console stubs and the
  monitor use; the early console and the panic path never depend on it.
- The implementation is **selected per build**, not probed at run time
  (architecture 36.11, M10-REFERENCE decision 12):

  | Image | Manifest | Input | Output |
  | --- | --- | --- | --- |
  | `console-v9958-hid` | `drivers/console/console-v9958-hid.json` | HID (class 4) | VIDEO (class 2) |
  | `console-serial` | `drivers/console/console-serial.json` | SERIAL (class 1) | SERIAL (class 1) |

  Production packs `console-v9958-hid`; the `console-serial` production
  variant packs the same image set with the other console
  (`make console-serial-selftest`).  Both declare their dependencies in
  the image manifest; the host packer resolves them to endpoint numbers
  before the image is packed, and the driver reads only those numbers
  from its boot bundle.
- There is **no runtime probe and no fallback**: each console operation
  forwards to the endpoint the build selected, and a required dependency
  the bundle does not resolve fails the driver's init.  A required driver
  that fails to load or initialize is a boot failure (36.11): the loader
  names it on the early console and the nucleus stops the boot there.
- Every operation is one forwarded call: the caller's descriptor travels
  down the chain, so a write moves the whole transfer through the console
  and its output driver without a staging copy here.  The console owns
  no buffer, no IRQ and no state beyond its two endpoint numbers.
- A missing endpoint number is reported as "no such driver"; the M8
  table of per-call error passthrough still describes the behavior, with
  the class error names replaced by the protocol's own.

| Call | No endpoint | Op unsupported | Device error |
| --- | --- | --- | --- |
| `console_putc` | no-driver error | unsupported-operation error | passed through |
| `console_write` | no-driver error | unsupported-operation error | byte count / error |
| `console_poll` | no-driver error | unsupported-operation error | 1/0 / error |
| `console_getc` | no-driver error | unsupported-operation error | byte / `ZOS_EAGAIN` |

Normal boot prints its acceptance line through the selected console
driver (the production build routes it to the V9958; the serial variant
to SIO0/B):

```text
console: driver OK
```

### 8.1 Console channels

Five distinct paths must not be confused:

- native VIDEO operations (`ZVIDEO_OP_SEND`): graphics/register
  operations to the video endpoint, independent of any text mode.
- V9958 character output (the video endpoint's write operation): the
  85x26 terminal renderer, consumed by `console-v9958-hid` and therefore
  by `console_putc`/`console_write` in the production build.  ESC `c`
  through this path runs the full display reset.
- HID character input (the HID endpoint): keyboard bytes fetched by
  `console-v9958-hid` from the input endpoint, decoded by the HID image
  from IOC replies.
- serial console (the SERIAL endpoint): the `console-serial` driver's
  both-direction path; the M7-normal binding.
- direct early SIO (`early_console_*`): the permanently resident,
  binding-independent path used by the boot banner, the target control
  transcripts and `panic()`.  It never consults a console driver, so it
  works even when the selected console is HID/V9958.

No console driver knows about the other implementations.  The
console-integration image `build-console-integration-selftest/` (which
absorbed the console-binding image in work package 42d) boots
`console-v9958-hid` and runs the unchanged M7 `monitor.zex` through that
pair.  It runs the package-20 target diagnostic and the ESC `c`
display-reset program (`tests/console/escc.c`, work package 42e), and is
driven by
`make smoke-hardware-console-integration` followed by the human monitor
pass.  The `console-serial` variant is validated by `make
console-serial-selftest` and `make smoke-hardware-console-serial`.

### 8.2 Failure-path evidence

The retired M3 fallback flags (`ZOS_M3_NO_STATIC_DRIVERS`,
`ZOS_M3_NO_CONSOLE_BIND`) no longer exist.  The M4 failure image that
proved failed-init rollback, the loader group's failed-init fixture and
the `sio_fail`/`sio-irq` container fixtures were all **deleted** in M10
(work packages 42b/42c) with the registry, the driver ABI and the arena
they exercised.

With build-selected consoles there is no composition fallback at all:
the store either provides the drivers the manifests require or the boot
fails.  The observable failure row is
`build-console-serial-fail-selftest/`, built and replayed by
`tests/test_console_serial.py`: its ROMFS manifest omits
`console-serial.zmod` while `drivers.man` still lists it, so the loader
prints

```text
driver loader: console-serial.zmod: no image
drivers: unavailable 0x05
console: driver failed
```

on the early console and halts before the launcher.  A load or bring-up
failure is never reported as `<name>: unavailable 0x<ZOS_E*>` any more:
the loader's own line (`docs/module-format.md` section 7) is the
diagnostic.  ROMFS corruption keeps the fatal recovery policy.

### 8.3 Console integration acceptance (package 20)

The package-20 integration gate keeps its shape - one production driver
set, one target diagnostic, one scripted monitor session - over the M10
boot path:

- **Boot contract**: `sio.zmod`, `ioc.zmod`, `hid.zmod`, `v9958.zmod`,
  the selected console image (`console-v9958-hid` in the integration
  image) and `monitor.zex` are the production store's entries, named by
  `drivers.man` in load order; the loader places them, installs the
  publications and runs their boot and init entries, and only then does
  the ESC `c` program run (it writes the display reset and its line, then
  execs the monitor).  A failed placement or init is a boot error with
  the loader's line, and ROMFS corruption keeps the fatal recovery
  policy.
- **Host/simulator evidence**: `make console-integration-selftest` builds
  the integration image (full driver set + the target diagnostic + the
  ESC `c` program + the monitor launcher) and replays the production
  bytes (`tests/test_console_acceptance.py`): the load order and the
  build-selected binding, IOC command/status/repeated-PING and the
  /SHARED READY/DONE bulk interlock, HID queue traffic with the
  stuck-doorbell penalty, the ESC `c` display reset (the V9958 bring-up
  sequence runs twice), the scripted monitor session with two Q
  exit/relaunch cycles, the malformed-request refusals, and the
  publication/endpoint invariants the packed containers carry.
  `make console-serial-selftest` replays the same set with
  `console-serial`: the monitor prompt, the typed line and the
  relaunch all on the serial lane (`tests/test_console_serial.py`).
- **Physical gate**: flash
  `build-console-integration-selftest/zephyros-console-integration-selftest.bin`,
  run `make smoke-hardware-console-integration` before reset, watch the
  ESC `c` display reset, perform the human HID/V9958 monitor pass, then
  flash the console-serial variant and run
  `make smoke-hardware-console-serial` with a serial terminal.  The
  package-20 validation record
  (`docs/validation/2026-10-03-m8-device-stack.md`) carries the M8 image
  hashes and the observed results; the M10 re-runs are recorded in
  `docs/validation/2026-10-09-m10-prune-nucleus.md` and
  `docs/validation/2026-10-10-m10-console-builds.md`.

## 9. Drivers and interrupts

A driver image claims an M2 logical source through the boot bundle's
`irq_claim` entry and receives a handler; the executive maps its bank
before dispatch and owns the final `RETI`.  The production SIO image
stays polling-only, and the IRQ integration is exercised on the subsystem
acceptance image (`tests/test_subsys_sim.py`):

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
CTC owner (`kernel/irq/ctc.c`) keeps only `platform_irq_quiesce()`;
`platform_ctc0_test_start()`, `platform_ctc0_stop()`, and the obsolete
`platform_irq_vector()` are not declared or linked in production.

## 10. Placement and validation

**M10 (work package 42c):** a driver is bank-6 driver code - its hot
segment in `6000h-DFFFh` (the mode-11 window), its boot/bulk segments in
`0000h-5FFFh` - placed by the resident loader at the addresses the host
packer assigned; the bank-7 arena the M4-M9 drivers occupied is deleted,
and nothing a driver owns may enter permanent common memory.  The
residency of every kernel object is the two-tier classification of
`config/residency.json` (architecture 36.5; `docs/memory.md` section 2.2).

The M8/M9 rules this replaced: all driver code, descriptors, ops tables
and state lived in bank 7 or in the loaded module's arena granules, with
the arena derived from the aligned linked-kernel end and the runtime-data
size and at least 16 KiB remaining.
`tools/mapcheck.py` enforces this with an explicit symbol list and a
prefix sweep over `_device_`, `_console_`, and `_m3_` symbols.  A
rejection sweep fails the build on any linked `_sio_`, `_sio0b_`,
`_sio_driver`, `_ioc_`, `_hid_`, or `_v9958_` symbol (the SIO, IOC, HID
and V9958 drivers may exist only as modules), and
`mapcheck.py --production` additionally rejects every acceptance symbol
(static-driver remnants, test runners/results, fixture catalogs, test
SIO module entries) in the production image.

Host validation layers:

- `tests/test_driver_images.py`, `tests/test_zmod.py` – the driver-image
  contract: the flat container, the plan the packer emits, the loader's
  bounds, endpoint numbering, dependency resolution and publications.
- `tests/test_source_layout.py` – the driver-facing include closure and
  the milestone-retention window (retired owners must stay deleted).
- `tests/test_subsys_sim.py` – replays the subsystem acceptance image in
  the Z80 simulator: the syscall and IRQ groups at the early hook, the
  ROMFS-reader and process/ZEX groups at the late hook (with injected CTC0
  vectors), and the scripted monitor session.
- `tests/test_romfs_sim.py` – the ROMFS reader/access replay and the
  corrupt-store image (`TestCorruptImageReplay`: panics with
  `PANIC: romfs: corrupt recovery image` before any module runs).
- `tests/test_hid.py` / `tests/test_hid_sim.py` – the HID class/driver
  contract and the real SIO+IOC+HID image replay (section 6).
- `tests/test_v9958.py` / `tests/test_v9958_sim.py` – the V9958 VIDEO
  class/driver contract, the font asset pipeline, and the real
  SIO+IOC+HID+V9958 image replay against the V9958 peer (section 7),
  plus the character-path coverage that moved out of the retired
  `test_v9958_console.py`.
- `make subsystem-selftest` – the mechanism acceptance image (its
  `test_romfs_sim.py` replay also patches a store header in place to
  prove the corrupt-store panic, which work package 42d folded out of
  the retired corrupt-store image).
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

No heap, no driver unload or hot-plug, no filesystem API for images, no
process API here, and no user-visible device syscall.  The block class M9
defined is deleted with no resident driver: the first block driver
arrives with its first consumer (architecture 36.4).  M4 stages 07-08 (this
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
package).  The M3 numeric registry model it describes is deleted (M10, work package
42c); the class numbers survive as the protocol types of section 12.

## 12. Resident driver protocols (M10, packages 41-41b)

The five driver images the production store carries are the SIO serial
transport, the IOC transport, HID input, the V9958 video core and the
console.  Each is a ZMOD driver image (`docs/module-format.md`,
host-linked at its final bank-6 address) placed by the resident
loader (`driver_load_all`, manifest `drivers.man`); the host packer
assigns each endpoint number and resolves the console's and HID's
symbolic dependencies, the loader installs the host-built tables, and each
image is entered only through `struct driver_boot` (the nucleus table
plus the resolved dependency numbers) - a driver never calls another
driver's symbols and never looks a peer up at run time.  `abi/zos/protocol.h`
is the whole contract; this section records the operation numbers, frames
and descriptor conventions the images implement.

Common shape.  Every operation is one `call(dependency_ref, op, frame)`:
`frame->arg0`/`arg1` are the operation's two words, `frame->payload` is
8 inline bytes, `frame->desc` is the nucleus-issued descriptor id (or
`ZOS_IPC_DESC_NONE`), and `frame->status` carries the result -0 or a
negative `ZOS_E*`/class error.  A descriptor-bearing operation also
passes `arg0` = offset into the caller's buffer and `arg1` = length, so
one call moves one bounded run.  Operations that return a single word
(the status queries and the byte-sized forms) publish it as the call's
return value.

Shared-memory staging.  A driver that has bytes to hand on names its own
driver-window buffer in the reply payload; the peer or the nucleus
finishes the transfer.  Two client classes exist, and both are stated by
the protocol:

- a *driver* client (HID, the console, the V9958 character path) is
  dispatched by the nucleus, so it may hold descriptors and may use
  `move` for same-domain staging (a driver-to-driver call is near-direct:
  the frame travels by reference and no bank switch happens).  This is
  the resident-domain convention;
- a *nucleus* client (the FS driver image's fixed-core companion and the
  nucleus's own boot probe) can hold neither descriptor nor `move`, so it
  stages bytes through the common copy gate in `ZOS_SYSCALL_STAGE_SIZE`
  chunks against the driver-window address the reply names.

### 12.1 SIO serial (class `ZOS_CLASS_SERIAL`, 1)

SIO0/B - the same channel as the early console - as a resident driver.
Polling only: the SIO interrupt mode stays disabled, so the serial
driver owns no IRQ and the early console keeps working before, and
without, any driver.

| op | number | frame | result |
|----|--------|-------|--------|
| `ZSIO_OP_PUTC`  | 0 | `arg0` = byte | 0 or `-ZOS_ETIMEDOUT` |
| `ZSIO_OP_WRITE` | 1 | desc IN, `arg0` offset, `arg1` length | bytes written |
| `ZSIO_OP_POLL`  | 2 | - | 1 when a byte is pending, else 0 |
| `ZSIO_OP_GETC`  | 3 | - | byte 0..255 or `-ZOS_EAGAIN` |
| `ZSIO_OP_READ`  | 4 | desc OUT, `arg0` offset, `arg1` length | bytes drained |

### 12.2 V9958 video (class `ZOS_CLASS_VIDEO`, 2)

The seven native VIDEO operations of `drivers/include/ioc_protocol.h` plus
  the
character path the console composes.  The display reset
(`ZVIDEO_RESET`) is a hot-segment path: it re-runs the full bring-up,
including the font and atlas upload, reading the font from the image's
own bulk segment through `bulk_read` (the boot/bulk tier is not mapped in
the mode-11 driver window).

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
`ZHID_OP_READY` reports whether the IOC link came up; no console driver
probes it any more (work package 42e: consoles are build-selected, and
`console-v9958-hid` requires HID by manifest).

| op | number | frame | result |
|----|--------|-------|--------|
| `ZHID_OP_READ_TEXT` | 0 | desc OUT, `arg0` offset, `arg1` maximum | bytes transferred (0 when empty) |
| `ZHID_OP_POLL`      | 1 | - | 1 when a byte is buffered/just delivered |
| `ZHID_OP_GETC`      | 2 | - | byte or `-ZOS_EAGAIN` |
| `ZHID_OP_READY`     | 3 | - | 1 when the IOC dependency is up |

### 12.4 IOC transport (class `ZOS_CLASS_IOC`, 5)

The command/reply lane and the bulk lanes of the Z80-to-MCU transport.
The driver owns wire framing, CRC, sequence, lane setup and the
READY/DONE bulk admission (`drivers/ioc/`); a client sees native
`struct zioc_frame` records (TYPE, STATUS, LENGTH, DATA[26]).

| op | number | frame | result |
|----|--------|-------|--------|
| `ZIOC_OP_COMMAND`    | 0 | `arg0` request type, `arg1` request DATA length (0..24), `payload[0..arg1-1]` request DATA | 0; reply `arg0` type, `arg1` DATA length, `payload[0..1]` staged reply-frame address, `payload[2]` reply STATUS, `payload[3..4]` bulk buffer address |
| `ZIOC_OP_REPLY`      | 1 | desc OUT, `arg0` offset, `arg1` length | bytes copied from the stored reply frame |
| `ZIOC_OP_BULK_READ`  | 2 | `arg1` length (1..512), optional desc OUT with `arg0` offset | length; without a descriptor `payload[0..1]` names the driver buffer |
| `ZIOC_OP_BULK_WRITE` | 3 | `arg1` length (1..512), optional desc IN with `arg0` offset | length |
| `ZIOC_OP_STATUS`     | 4 | - | semantic status word (`ZIOC_STATUS_*`) |
| `ZIOC_OP_READY`      | 5 | - | 1 when the link is up and no recovery is pending |

The 29-byte native request frame does not fit the 24-byte inline payload,
and no deployed request class exceeds 24 DATA bytes, so the exchange
control travels in the frame words and `payload[3..4]` reports where the
bulk buffer is: a descriptor-less (bank-7) client writes the bytes it
wants written to that address through the copy gate and then issues
`ZIOC_OP_BULK_WRITE` without a descriptor.  The FS driver image's
fixed-core companion did exactly that for the IOC-FAT provider; work
packages 42b/42c deleted it with the arena module loader it lived in.

### 12.5 Console (class `ZOS_CLASS_CONSOLE`, 6)

The console forwards to the drivers its build selected (work package
42e): `console-v9958-hid` maps input to HID and output to VIDEO, and
`console-serial` maps both directions to SERIAL.  The host packer
resolves each manifest's symbolic dependencies to endpoint numbers; the
driver reads them from its boot bundle, requires them (a zero fails its
init and the boot, 36.11) and never probes a peer at run time.  It is the
endpoint the SDK console functions and the monitor use; the early console
and the panic path do not depend on it.

| op | number | frame | result |
|----|--------|-------|--------|
| `ZCONSOLE_OP_WRITE` | 0 | desc IN, `arg0` offset, `arg1` length | bytes written |
| `ZCONSOLE_OP_READ`  | 1 | desc OUT, `arg0` offset, `arg1` length | bytes drained (non-blocking) |
| `ZCONSOLE_OP_GETC`  | 2 | - | byte or `-ZOS_EAGAIN` |
| `ZCONSOLE_OP_POLL`  | 3 | - | 1 when input is pending |
| `ZCONSOLE_OP_PUTC`  | 4 | `arg0` = byte | 1 or negative |

`ZCONSOLE_OP_PUTC` is the descriptor-less form: `putchar()` uses it (one
call, no copy), and the nucleus uses it for the boot transcript it prints
through the console driver.  A store that lists no console driver makes
the SDK console functions fail with `-ZOS_ENOENT` and leaves the boot
transcript on the early console.

### 12.6 Placement, IRQs and ledgers

The six images of the production store own bank 6's two resident tiers
(architecture 36.6, 36.20): the host packer links each segment at its
final address, and the hot segments (`_CODE` + `_DRIVER_IO`, the one
bank-6 range the mode-11 window and the mode-10 aperture both show) total
21,846 bytes of the 32,768-byte hot tier; the boot segments plus the
V9958 font bulk segment total 2,170 bytes of the 24,576-byte boot/bulk
tier.  The bank-7 driver arena is deleted (work package 42c): the IOC-FAT
provider is the last image of the bank-6 set, not an arena occupant.
No production driver image owns an IRQ: SIO0/B is polled, the IOC and HID
paths are polled through the same lane, and the V9958 is register-driven.
The driver-owned IRQ path itself is covered by the acceptance image's
synthetic `irqx` driver (work package 39).  The ledger lines the build
prints are `driver hot: ...` / `driver boot/bulk: ...` with one row per
image (see `docs/memory.md`).

### 12.7 The `/dev` publication table (package 42a)

Programs never name an endpoint.  A driver's *publication* is a namespace
name under `/dev` plus the 8-bit protocol type of the instance that serves
it, and the whole mapping is built on the host (architecture 36.13,
decision 13):

1. A driver manifest may carry `publishes` rows - the name *below* the
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
   (`docs/module-format.md` section 6), 20 bytes per row, ordered after
   the segment, endpoint and dependency tables in the driver section.
3. The loader validates each row (name shape, protocol type in range,
   endpoint number non-zero) and installs it in the DEVICE provider's
   table.  The provider synthesizes the grouping rows from the names it
   received - `/dev/serial/0` implies the container `/dev/serial` - so the
   table on the target always matches the packed store.

From a program the model is two calls (`docs/abi.md` section 12.1):

```c
unsigned char type;
int handle = bind("/dev/serial/0", 0, &type);   /* type = SERIAL */
call(handle, op, &req);                          /* one frame     */
unbind(handle);
```

`bind` returns the handle *and* the type the publication declares: a
client that cares checks it, and there is no expected-type argument and no
`handle_info`.  A grouping name (`/dev/input`) enumerates but refuses to
bind with `-ZOS_EISDIR`.  Names are resolved case-insensitively, and the
lookup is a table scan in the provider - the host has already done every
decision that could be taken at build time.

The production build's five manifests still carry driver ABI 3 (no
publications) and keep the stage-3 lookup path: the DEVICE provider, the
table and the client API are linked by the `namespace-selftest` acceptance
image (`make ns-selftest`), where the SIO and console manifests declare
`/dev/serial/0` and `/dev/console` and a synthetic publisher adds
`/dev/input/keyboard` and `/dev/video`.  Package 42b turns the production
manifests over and deletes the lookup path
(`docs/validation/2026-10-09-m10-namespace-handles.md`).
