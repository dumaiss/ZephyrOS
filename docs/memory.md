# KonaOS Memory Map

This document describes the Zephyr-80 Mk2 memory layout built by
`make BOARD=mk2`. The LS612 DAT maps sixteen logical 8 KiB regions through
one of two eight-entry contexts. The address map, boot rules and execution
model are defined in [architecture.md](architecture.md#27-zephyr-80-mk2-the-ls612-dat),
sections 2.7.2–2.7.6.

## 1. Physical memory

The initial Mk2 configuration has 512 KiB of ROM and 512 KiB of SRAM,
divided into 128 physical pages of 8 KiB each:

| physical pages | physical addresses | backing | use |
|---|---|---|---|
| `00h–3Fh` | `00000h–7FFFFh` | ROM | boot image and recovery ROMFS |
| `40h–7Fh` | `80000h–FFFFFh` | SRAM | nucleus, drivers, processes, common page and allocator pool |

An enabled DAT translates a logical address with
`physical = (page << 13) | (logical & 1FFFh)`. Each entry contains a
seven-bit physical page number. ROM writes are ignored; there is no RAM
shadow behind ROM.

### 1.1 Logical map and system DAT

The system DAT is the nucleus view. Entries 7 and 15 refer to the same
physical common page, so the common region remains mapped when the DAT
bank changes.

| entry | logical addresses | system mapping | normal role |
|---:|---|---|---|
| 0 | `0000h–1FFFh` | SRAM page `40h` | system page-zero stubs; temporarily caller page zero for a syscall |
| 1 | `2000h–3FFFh` | SRAM page `41h` | fixed nucleus code/data |
| 2 | `4000h–5FFFh` | SRAM page `42h` | fixed nucleus code/data, driver and ISR stacks |
| 3 | `6000h–7FFFh` | SRAM page `43h` | cold-set page or driver-domain page 3 |
| 4 | `8000h–9FFFh` | SRAM page `44h` | cold-set page or driver-domain page 4 |
| 5 | `A000h–BFFFh` | copy source window | remapped for page copies, or driver-domain page 5 |
| 6 | `C000h–DFFFh` | copy destination window | remapped for page copies, or driver-domain page 6 |
| 7 | `E000h–FFFFh` | SRAM page `7Fh` | common page; also working entry 15 |

At the boot handoff the system entries are `40h 41h 42h 43h 44h 00h
00h 7Fh`. Entries 5 and 6 are placeholders until a copy or driver call
uses them. The working DAT initially contains the same placeholder; `exec`
loads the selected process image before entering it. Entry 0 is remapped
for syscall/launch crossings and for driver boot or bulk-map spans. Entries
3–6 hold a cold set during a cold call, the copy windows during copies, or
the driver's hot domain during a driver call.

The mapping control state and sixteen software mirror bytes live at
`EF10h–EF22h` in common memory (`KOS_DAT_STATE_*` in
`platform/zephyr80mk2/private/memory.inc`). Code updates the mirror before
writing the mapper. NMI handlers do not change the mapping.

### 1.2 ROM image and recovery store

The ROM image is 512 KiB, or 64 physical 8 KiB pages:

| ROM pages | contents |
|---|---|
| `00h` | reset and stackless bootstrap view |
| `01h–04h` | fixed nucleus and cold set 0 image |
| `05h–06h` | cold set 1 image |
| `07h` | common page image |
| `08h–2Bh` | recovery ROMFS, 36 pages |
| `2Ch–3Fh` | available ROM capacity |

The recovery store provides 288 KiB (`KOS_ROMFS_CAPACITY`). The reader
maps its source page into the system copy window and copies directly into
the destination pages. ROMFS file data is not accessed through a Mk1
mode-00 shadow mechanism.

## 2. Linked nucleus regions

The Mk2 linker lays out the nucleus in logical address space. The physical
pages backing the logical regions are listed in section 1.1. The fixed
region and the two cold sets are separate linked areas; a cold set is
mapped at system entries 3–4 only while its code runs.

| logical addresses | linked contents |
|---|---|
| `0000h–00FFh` | low entry stubs in each page-zero image; process images also carry the far-window vector at `0028h` |
| `0100h–1FFFh` | caller workspace, code/data and syscall argument blocks |
| `2000h–5CFFh` | fixed nucleus (`_CODE`, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL`, `_DATA`, `_INITIALIZED`) |
| `5D00h–5EFFh` | 512-byte driver stack |
| `5F00h–5FFFh` | 256-byte ISR stack |
| `6000h–9FFFh` | cold-set link window (`_WINDOW`, `_WINDOW_DATA`); two 16 KiB sets are placed in distinct physical page pairs |
| `A000h–DFFFh` | copy windows in the system DAT; no fixed nucleus code or data is linked here |
| `E000h–FFFFh` | shared common page |

Set 0 is linked at `6000h–9FFFh`. Set 1 is linked at `16000h–19FFFh`
and executes at the same low 16-bit addresses when mapped. Further sets
would follow the same `set n * 10000h + 6000h` link convention, but the
current image defines two. The build generates fixed-region veneers for
calls that cross into a set, rejects external data references into a set,
and checks that driver-reachable code does not depend on a cold set.

### 2.1 Common page and stacks

The common page is physical SRAM page `7Fh`, visible at `E000h–FFFFh` in
both DAT banks. `_COMMON` and `_COMMON_DATA` have a combined ceiling of
3,072 bytes beginning at `E000h`; other fixed reservations in the page
are outside that packed run.

| addresses | reservation |
|---|---|
| `E000h–EBFFh` | common code/data packed run, bounded by `KOS_COMMON_CEILING` |
| `EF08h–EF0Ah` | real-time interrupt vector (`JP` to the owner's handler) |
| `EF0Bh` | idle `RETN` target |
| `EF10h–EF22h` | DAT mirrors and cached working-image identity |
| `F000h–FCFFh` | real-time region lent to the active RT owner |
| `FD00h–FDFFh` | IM2 vector table (`I = FDh`) |
| `FE00h` | vector-`FFh` guard byte |
| `FE01h–FE07h` | IRQ transition state |
| `FE08h–FE87h` | 128-byte IRQ transition stack |
| `FE88h–FFFFh` | common boot/runtime stack; initial `SP = 0000h` grows down |

The syscall gate stack is 384 bytes in the common packed area. The driver
stack is at `5D00h–5EFFh`; the ISR stack is at `5F00h–5FFFh`. The IRQ
vector page, guard, state and transition stack are described by
`platform/zephyr80mk2/private/irq.inc` and `irq.h`.

### 2.2 Linker areas and runtime initialization

`Makefile` links the Mk2 image with `_CODE=2000h`, `_WINDOW=6000h`,
`_COLD1=16000h` and `_COMMON=E000h`. Geometry derivation places fixed and
cold data after the respective code, subject to the driver/ISR stack and
common-page boundaries.

| area | placement | purpose |
|---|---|---|
| `_BOOT` / reset image | ROM page 0 | stackless ROM bootstrap |
| `_CODE`, `_HOME`, `_INITIALIZER`, `_GSINIT`, `_GSFINAL` | fixed region | fixed nucleus code and loadable initializers |
| `_DATA`, `_INITIALIZED` | derived fixed-region base | fixed runtime data |
| `_WINDOW`, `_WINDOW_DATA` | cold set 0 at `6000h` | set 0 code and data |
| `_COLD1`, `_COLD1_DATA` | cold set 1 at `16000h` | set 1 code and data |
| `_COMMON`, `_COMMON_DATA` | common page at `E000h` | gates, transition state, early console, IRQ and event paths |

The bootstrap copies ROM pages into SRAM pages before handoff. The C
runtime copies initialized data and zeros the fixed data, both cold-set
data areas, and the explicitly initialized common state. The common page
is not implicitly zeroed as a single region; subsystems such as the event
core initialize their own state.

## 3. Page ownership and execution

`kernel/memory/mk2/pages.c` tracks physical SRAM page ownership. At boot,
the image reserves the system page-zero image (`40h`), fixed nucleus
pages (`41h–42h`), cold-set pages (`43h–46h`) and common page (`7Fh`).
The driver loader allocates its page domain as nucleus-owned pages. ROM
pages are never allocated. Other SRAM pages begin free and can be
allocated to a process; allocation is all-or-none and takes the lowest
available pages. Process exit releases its owned pages and objects.

Each process has one eight-byte working DAT image in common state. Entries
0–6 map its seven primary pages; entry 7 maps the common page. The image
can be restored from the software copy when the working DAT cache does
not already hold it. A caller's current mapping, including an open far
window, is captured across syscalls and IPC.

`exec` checks the executable's board field, takes a process slot and seven
primary pages, builds its page-zero stubs and startup block, loads the
primary image and BSS, then allocates and loads each far/data segment as
an extended-memory object. The page-zero image contains the syscall and
NMI stubs and the far-window vector (`JP kos_far_map` at `0028h`). Only
the BSS and specified startup regions are zeroed.

The ROMFS loader maps ROM source pages through the copy windows. A
program's FILE-backed load reads through the normal filesystem path.
Process return releases the child's pages and objects, invalidates the
cached DAT identity, and restores the parent's exact working mapping.

## 4. Resident driver domain

The drivers retain their established linked layout, but their code and
data reside in a nucleus-owned set of up to seven physical pages. Domain
logical page `n` contains the bytes linked at `n * 2000h`. The loader
allocates pages as the driver segments require them, copies the images
from ROMFS through the copy windows, zeroes BSS, and initializes the
driver with its domain mapped.

For a driver call, handler or init routine, domain pages 3–6 occupy system
entries 3–6 (`6000h–DFFFh`). The driver stack is used for the outermost
call. Driver-to-driver calls remain directly mapped, and shared driver
buffers stay addressable. The current production driver hot tier spans
32 KiB, so a 16 KiB window would not contain the linked driver set.
During a bulk-map span, the driver's page zero is mapped at system entry
0. The fixed IRQ trampoline maps the driver domain for a handler and
restores the interrupted DAT state afterward.

The driver API may call only fixed-region or common code. The Mk2
residency checker enforces this rule because entries 3–6 hold the driver
domain while driver code executes.

## 5. Copy windows and far window

### 5.1 Copy windows

System entries 5 and 6 are the source and destination windows at `A000h`
and `C000h`. `kernel/memory/mk2/page_copy.asm` maps them to physical pages,
updates the software mirror before each mapper write, and copies a bounded
chunk with an unrolled `LDI` run (`kernel/runtime/fastcopy.inc`) or
zero-fills it. Copies split at page boundaries. No interrupt path depends
on these entries, so a copy preserves the caller's interrupt state. On the
simulator a 4,096-byte `XMEM_READ` between a high buffer and an unmapped
object costs 74,059 T-states, `RST 30h` through return (about 16
T-states a byte in the copy itself, `tests/perf_os_sdk.py`).

IPC `copy_in` and `copy_out` resolve a program buffer through the
caller's current working DAT image, including far mappings, then copy
directly between caller and driver pages. `bulk_copy_out` does the same
from a driver's bulk extent, so a block-cache hit reaches the caller in
one pass. Nucleus buffers are addressed directly. There is no Mk1
bank-path selection or common-memory staging for these copies. A
page-0 buffer at `0105h-1FFFh` is instead lent to the driver through
system entry 0 (`borrow`, architecture 4.9), which already shows it, so a
device receive lands in it with no copy.

### 5.2 Far window and extended-memory objects

The SDK far window is the logical range `8000h–BFFFh`, working entries
12–13. `kos_far_map` maps the selected object's pages there and returns
the previous one-byte mapping handle. Handle zero selects the program's
own pages. `RST 28h` reaches the far-window vector in every process page
zero; opening the window uses this vector rather than a syscall.

An extended-memory object is a list of physical pages owned by its
process. An object can use up to seven pages; a mappable object uses at
most the two pages visible in the far window. Far-code and far/data
segments are objects too. `SEG_INFO` and `SEG_MAP` describe and map them;
the format's segment limits and the object table still apply. Freeing an
object that occupies the active far window closes that window before the
pages are released. No bank-class, per-bank object limit or aperture-slice
rule applies to Mk2.

## 6. Map and residency checks

`config/residency-mk2.json` defines the fixed region, cold sets and
driver-reachable object list. `tools/mapcheck.py mk2 --link` rewrites
cross-set calls through generated veneers; `tools/mapcheck.py mk2`
validates the linked image and its residency map. It checks:

- fixed-region, stack, cold-set, copy-window and common-page bounds;
- every object is placed once and every cold set is declared;
- no code or data occupies system entries 5–6 as fixed nucleus content;
- driver-reachable objects do not reference cold sets;
- every nucleus stack lies in system entries that are never remapped,
  and no driver's private stack lies in an entry a driver API service
  remaps on the caller's stack (`driver_call_remaps`, with `--drivers`
  and the driver plan);
- external references into cold-set data are rejected;
- DAT image rows, gate state and far-table sizing match process/object
  limits; and
- the packed common run stays within the 3,072-byte ceiling.

`tools/pack_image.py --board mk2` checks the 512 KiB image, the ROMFS
placement and page-image boundaries. `tests/test_source_layout.py` checks
that executable mapping code stays in kernel/boot operations and that
the C and assembly platform constants agree. The Mk2 operation definitions
are in `kernel/memory/mk2/map_ops.inc`; mapping is controlled through the
Mk2 DAT operations.

## 7. Platform constants

The C and assembly descriptions are value-checked in `tests/test_source_layout.py`:

| files | contents |
|---|---|
| `platform/zephyr80mk2/public/hardware.h`, `.inc` | peripheral ports and register values |
| `platform/zephyr80mk2/private/memory.h`, `.inc` | page, linker, stack, ROMFS, DAT and window geometry |
| `platform/zephyr80mk2/private/irq.h`, `.inc` | IM2 and IRQ transition reservations |
| `platform/zephyr80mk2/config.mk` | Mk2 include roots and build configuration |

The Mk2 platform files describe constants and configuration. Mapping
operations and emitted instructions remain in the boot and kernel memory
code.
