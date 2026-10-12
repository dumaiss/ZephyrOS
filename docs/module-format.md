# ZMOD driver image format and the resident loader

**Status: M10, work package 42c.**  There is exactly one ZMOD form: the
**driver image**, a flat load plan the host packer links and the nucleus
loader copies.  The arena module loader (`kernel/module/loader.c`,
`relocate.c`, `arena.asm`), the relocation-record container, the module
types, the import tables and the host `emit-asm` test fixtures are
**deleted**; the record of that model is the M4-M9 history in
`docs/architecture.md` (sections 22-31) and `docs/validation/`.  The file
keeps its historical name because the container is still the ZMOD
container; the format it describes is now the driver form only.

The split of work is the M10 rule (M10-REFERENCE decision 2): **the host
computes everything.**  `tools/zmodpack.py` mini-links the declared
objects, links every segment at its final bank-6 address, applies the
`ZREL_ABS16` records on the host, assigns endpoint numbers, resolves
inter-driver dependencies and validates the whole container; the nucleus
loader (`kernel/driver/driver_loader.c`) copies the finished images and
checks only the bounds that protect the nucleus and the fixed driver-bank
layout.  Nothing is linked, patched or version-checked at boot.

The architectural contract is `docs/architecture.md` sections 36.6
(driver bank), 36.11 (resident images and host assembly), 36.12 (boot),
36.13 (namespace providers) and 36.20 (ledgers).  The concrete layout is
pinned by `formats/include/zmod.h` and by the compile-time assertions
the target build enforces.

---

## 1. Container layout

A container is a packed little-endian byte stream: a 16-byte header, the
four tables in a fixed order, then the payload.

```text
offset 0                 header, 16 bytes
offset 16                segment table,    12 * segment_count
after the segments       endpoint table,    8 * endpoint_count
after the endpoints      dependency table,  4 * dependency_count
after the dependencies   publication table, 20 * publication_count
payload_offset           segment images, concatenated in table order
total_size = payload_offset + sum(image_size)
```

`payload_offset` must equal the canonical table end
(`16 + 12*sc + 8*ec + 4*dc + 20*pc`) and `total_size` must equal the exact
container length: the loader and the packer both refuse a container whose
tables and payload do not end at those bytes.  The mandatory payload order
is what lets the loader read the container once, front to back, without a
record index.

Header (16 bytes; `formats/include/zmod.h` `struct zmod_header`):

| Offset | Field | Meaning |
| --- | --- | --- |
| 00 | `magic[4]` | `"ZMOD"` |
| 04 | `segment_count` | 1..4, `hot` required |
| 05 | `endpoint_count` | 0..8 |
| 06 | `dependency_count` | 0..8 |
| 07 | `publication_count` | 0..12 |
| 08 | `init_entry` | absolute hot-tier init entry, `FFFFh` = none |
| 0A | `payload_offset` | first payload byte (canonical table end) |
| 0C | `total_size` | exact container length |
| 0E | `reserved` | 0 |

There is no version field, no module type, no import table, no `fini`
entry and no relocation record.  A driver image has no runtime format
negotiation: an image that does not match this layout is a pack-time
error, not a boot error.

## 2. Segments and the bank-6 tiers

One record per role, at most one segment per role.  `base` is the final
bank-6 address the host assigned; the image is
`payload[payload_offset .. payload_offset + image_size)`, the BSS area
follows the image at `base + image_size`.

```text
segment  role:u8 0:u8 base:u16 image_size:u16 bss_size:u16
         entry:u16 payload_offset:u16
```

| Role | Destination | Visibility, and what runs there |
| --- | --- | --- |
| `hot` | bank 6 `6000h-DFFFh` | the mode-11 window (P = 1): all run-time code and data, plus the init entry |
| `io` | bank 6 `8000h-BFFFh` | `DRIVER_IO` buffers, the sources and targets of `copy_in`/`copy_out` |
| `boot` | bank 6 `0000h-5FFFh` | boot-only code, run once through the mode-10 gate with Q = 6, never again |
| `bulk` | bank 6 `0000h-5FFFh` | bulk data, reached only by nucleus copies |

`entry` is the absolute boot entry and only the `boot` role may carry one
(`FFFFh` otherwise); `init_entry` in the header is the absolute hot
init the loader calls once after the boot entry.  Both must lie inside
their own segment's image.

Because every address in the payload is final, a segment may only
reference addresses the mode-11 window can still see: hot code and data
sit in bank 6 `6000h-DFFFh` and may not point into `boot` or `bulk` code.
`bulk` carries no relocation records at all.  The `io` block is allocated
first (see section 5), so the hot tier keeps its low addresses for code
and its `8000h-BFFFh` window for buffers.

## 3. Endpoints

```text
endpoint  class:u8 instance:u8 version:u8 flags:u8
          number:u16 dispatch:u16
```

`(class, instance)` is the protocol identity (the classes of
`abi/zos/object.h`); `version` is the image's own protocol version, kept
in the record but never checked at boot; `flags` bit 0 is the
re-entrancy declaration (`ZOS_IPC_EP_REENTRANT`); `number` is the
host-assigned endpoint number (`1..N` across the load order) and
`dispatch` the absolute hot-tier dispatch entry inside the hot image.

The host assigns numbers in load order, so the numbering of an image
depends on its position in the set, not on its name.  `number` 0 is
invalid everywhere: the loader refuses an endpoint record whose number is
zero, and installs the rest into the nucleus endpoint table before any
driver code runs.  There is no run-time registration, lookup or
unregistration path.

## 4. Dependencies

```text
dependency  class:u8 instance:u8 number:u16
```

A manifest declares dependencies symbolically; the packer resolves each
one to the endpoint number of an export in an earlier image or in the
image itself.  A missing *required* dependency is a pack error.  A
missing *optional* one resolves to `0`, which a driver reads as "this
build selected no such peer" (optional dependencies survive only in
acceptance doubles; the production console drivers declare every
dependency required).  The resolved numbers are installed into the
fixed-core array
the boot bundle names (`struct driver_boot`, `drivers/include/
driver.h`), one byte per declaration in manifest order; no driver looks a
peer up at run time.

## 5. Placement and the two ledgers

The packer allocates; the loader verifies.  The policy, applied when an
image is packed against its `--set`:

1. every image's `io` segment packs contiguously **down from `C000h`**,
   in load order - the whole `[io_low, C000h)` block is known before any
   code is placed, and a block that would leave `8000h-BFFFh` fails the
   pack;
2. the `hot` segments take the lowest free run of the hot tier,
   largest first, on one side of the io block (below it when it fits,
   otherwise just above `C000h`);
3. `boot` segments bump up from `0000h` and `bulk` data down from
   `6000h`, in load order.

A set that packs therefore always places.  The loader re-checks the
result, because the container arrives from the recovery store: each
segment's `base` and size must leave the tier of its role, `entry` and
`init_entry` must stay inside their images, each endpoint `dispatch`
inside the hot image and each dependency number inside what the nucleus
has installed.

Both sides report the same two architecture-36.20 ledgers:

```text
driver hot: <bytes> of 32768          hot + io bytes of the 6000h-DFFFh tier
driver boot/bulk: <bytes> of 24576    boot + bulk bytes of the 0000h-5FFFh tier
```

The loader prints them in acceptance builds; production takes them from
the packer's report (`drivers.plan.json`), which is what
`tools/pack_image.py` folds into the bank-6 part of the image manifest.
Each placed image is reported as `driver <name>: hot <addr> [io <addr>]
[boot <addr>] [bulk <addr>]` - the same line `zmodpack.py drivers`
prints - and the run ends with
`driver loader: <n> images in bank 6`.

## 6. Publications

A publication is a namespace name below the **DEVICE provider's** `/dev`
root, or the `/fs` root of a filesystem provider, that the image serves
(architecture 36.13):

```text
publication  name[16] type:u8 number:u8 object:u8 flags:u8
```

`name` is the provider-relative remainder of the path, NUL-terminated and
zero-padded; `type` is the 8-bit protocol type (`ZOS_TYPE_*`, listed in
`tools/zmodpack.py`); `number` is the host-assigned number of the
endpoint that serves it; `object` is the provider's own object identity,
handed back with every call.  `flags` is 0.

A grouping name is never published: the packer counts the derived rows
(`/input`, `/serial`) and the DEVICE provider synthesizes them from the
names it installs.  The packer refuses a name that is the root, longer
than the 15 provider-relative bytes a record holds, non-printable, or
duplicated anywhere in the set (compared case-insensitively), and refuses
more than `ZMOD_MAX_PUBLICATIONS` (12) rows per manifest, an unknown type
name, a type outside 1..255, an object above 255, or a set whose rows
plus the derived grouping rows overflow the nucleus publication table.

An image with no publications carries ABI 3 byte for byte (count 0, empty
table); one with publications carries ABI 4.  The publications of the
whole set are installed before the first driver's boot entry runs.

## 7. Boot manifest and the boot path

`drivers.man` (`ZOS_DRIVER_MANIFEST_NAME`) is a flat ROMFS `DATA` file
of 24-byte records - the ROMFS name field exactly, NUL-terminated and
zero-padded - naming the packed containers in load order.  The loader
reads it with the bootstrap ROMFS reader (`kernel/romfs`, fixed-core
since package 38) and requires the file size to be a multiple of the
record size.  At most `ZMOD_MAX_IMAGES` (8) images are placed.

Per image, in manifest order (`kernel/driver/driver_loader.c`):

1. copy the segment images to their host-assigned bases and zero their
   BSS through the common application-bank copy gate;
2. install the endpoint records into the nucleus endpoint table and the
   publications into the namespace;
3. run the boot entry once through the mode-10 gate with Q = 6;
4. run the hot init entry once through the fixed upcall shim with the
   boot bundle in `HL`; the init's `uint8_t` return is its status (the
   shim publishes it), and a non-zero status refuses the image - a
   required driver that fails to initialize is a boot failure (36.11).

Boot entry code runs with bank 7 unmapped: it may touch bank 6, the
common region and I/O ports, and must not call the bootstrap interface.
The hot init runs with the window open and has the interface table and
its resolved dependency numbers.

A failure is a boot error and prints one line on the early console:
`driver loader: <name>: <what>`, where `<what>` is `no image` (the
manifest named a file the store does not hold), `copy` (a segment that
does not fit its tier or a copy the gate rejected), `endpoint` (the
endpoint table is full), `publication` (the namespace refused the row),
or `invalid` (corrupt or truncated container, or an init status that is
not 0 - the same word for a
manifest whose size is not a multiple of the record size).  The nucleus
then prints `drivers: unavailable 0x<code>` and stops the boot on the
early console: a required driver that fails to load or initialize is an
explicit boot failure, never a fallback (36.11, work package 42e).  The
words are
short because the loader's byte budget is an architectural resource
(section 8).  Drivers are permanent (M10
decision 11): no unload, no restart, no fini entry, and a second
`driver_load_all` call is a no-op.

## 8. Loader size

The loader's byte cost is an architectural resource (36.3): it shares the
fixed core with the ABI, the copy primitives and the IRQ executive.  The
42c loader measures **1,970 bytes** (1,969 code + 1 data) against the
2,000-byte target of the 41b review, down from 5,122 bytes at the M9
source - it no longer parses or re-validates container tables the packer
already validated.  The evidence lines (`driver <name>:`, the two
ledgers) are acceptance-only; production links the error lines only.

## 9. Packer commands, manifest and reports

```sh
python3 tools/zmodpack.py pack <manifest.json> <out.zmod> \
    --objdir DIR --set MANIFEST [...] --report FILE
python3 tools/zmodpack.py validate <container.zmod> [...]
python3 tools/zmodpack.py dump <container.zmod> [...]
python3 tools/zmodpack.py drivers <out.man> <container.zmod> [...] \
    --report drivers.plan.json
```

`pack` for a driver manifest requires `--set`, the whole load order
including this image: the packer links this image against the set (one
plan, one endpoint numbering, one dependency resolution), so an image
rebuilt alone still matches its siblings.  `validate` and `dump` both run
`parse_container`, the host's authoritative validation.

A manifest names its objects, its `init` entry, its `segments` (role,
`image_areas`, `bss_areas`, optional boot `entry`), its `endpoints`
(`class`, `instance`, `version`, `dispatch` symbol, `flags`), its
`publishes` rows and its `depends` records (`class`, `instance`,
optional `optional`):

```json
{
  "name": "ioc",
  "objects": ["ioc.rel", "ioc_boot.rel", "sdcc_indirect_calls.rel"],
  "init": "ioc_driver_init",
  "segments": [
    {"role": "hot",  "image_areas": ["_CODE"],
     "bss_areas": ["_DRIVER_IO"]},
    {"role": "boot", "image_areas": ["_BOOT"],
     "entry": "ioc_boot_entry"}
  ],
  "endpoints": [
    {"class": 5, "instance": 0, "version": 1,
     "dispatch": "ioc_driver_dispatch", "flags": 0}
  ],
  "publishes": [],
  "depends": []
}
```

The per-image report carries each segment's role, final base, image and
BSS sizes and payload offset, every symbol's segment-relative offset with
its segment (`symbols`, `symbol_segments`), the endpoint numbers with
their absolute dispatch entries, and the resolved dependency numbers.
The set report (`drivers.plan.json`) carries the load order, the bases
per image and the two tier ledgers; `tools/pack_image.py` reads it for the
bank-6 ledger of the image manifest.

## 10. Retired with the arena loader

Work package 42c removed, with the format that used them: the relocation
records and every runtime relocation target; the header's format-version
byte, module type, fini entry and granule requirement; the driver arena,
its linker symbols and its granule plan; the import and resource-import
tables; the kernel driver-entry import table; every driver ops table; the
device class/capability model with its registration call; the filesystem
provider ops-table interface and its registration path; and the arena
loader's self-test fixtures (the packer's fixture emitter,
`tests/modules/`).  Bytes removed per item and the before/after ledgers
are recorded in `docs/validation/2026-10-09-m10-prune-nucleus.md`; the
history of the delete-and-reconcile pass is in
`work/42c-m10-prune-nucleus/progress.md`.
