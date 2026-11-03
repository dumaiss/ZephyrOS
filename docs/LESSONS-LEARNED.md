# KonaOS technical lessons learned

This is a technical retrospective of KonaOS through 2026-11-01. It focuses on
engineering problems that recurred or changed design decisions, what the
project learned from them, and what a developer can do to avoid repeating
them. Each lesson separates **observed evidence** from **inference**. The
validation records linked here contain detailed measurements, commands,
images, and acceptance limits.

This document is a source for future developer and alteration-guide material;
it does not replace the current architecture, ABI, SDK, device, memory, or
format references.

## 1. Budget the linked system, not the feature in isolation

**Observed.** M9's bank-7 ledger reached zero free granules when IOC-FAT was
first integrated. The provider was trimmed and the end-of-milestone slack
target revised from 2 KiB to 1 KiB by an explicit owner decision. Later, the
M10 design spike measured a 4.7 KiB increase in bank-7 code plus data for the
proposed facilities; the existing placement could not absorb it. Feasibility
measurements and a fixed-core/cold-half redesign kept the OS within its
two-bank rule. The current system tracks multiple separate ledgers, including
fixed, cold, driver hot, driver bulk, common, and stack areas.

Evidence: [M9 budget checkpoint](validation/2026-10-04-m9-bank7-budget.md),
[M10 design spike](validation/2026-10-06-m10-design-spike.md),
[two-bank feasibility](validation/2026-10-07-m10-two-bank-feasibility.md),
[memory map and ledgers](memory.md).

**Inference / guidance.** A source-file size estimate misses linker placement,
runtime data, alignment, call reachability, stack, boot-set, and other
consumers of the same constrained region. Before committing to a feature,
build a representative linked image and record each relevant ledger delta.
Make a spike answer feasibility questions when the result could force an
architecture change. Do not treat unused space in one region as available
space in another.

## 2. Increasing a resource limit changes handle contracts everywhere

**Observed.** Raising the system-wide extended-memory object limit from 16 to
32 enabled KonaEdit's File > Open flow while another document was open. That
change required 9-bit object handles (`0x000`–`0x1FF`), a widened store in the
SDK's `farmap_handles`, kernel lookup-bound changes, and audits of consumers.
The test proves the 33rd allocation fails, a handle above `0xFF` works, a freed
index can be reused with a new generation, and stale or byte-truncated handles
are rejected. The object pool is system-wide and also counts runtime objects
and executable segments, not only objects a single application allocates.

Evidence: [xmem object limit and handle audit](validation/2026-11-01-xmem-objects.md),
[architecture object rules](architecture.md#48-memory-application-banks-and-extended-memory),
[application integration finding](validation/2026-11-01-apps-mouse.md).

**Inference / guidance.** Capacity changes are interface changes even when
the public C type already appears wide enough. Audit storage, serialization,
generation bits, sentinels, loops, test fixtures, and all API boundaries for
truncation or old hard-coded limits. Test the upper valid value, first invalid
value, reuse, and stale reference. Test the real application workflow that
exhausted the limit; a unit allocation test alone would not have found the
KonaEdit failure.

## 3. Code and data placement are runtime correctness rules

**Observed.** KonaOS maps a 16 KiB aperture for far code or mappable data.
While it is open, code and data needed by that execution path must remain
accessible. Placed application builds therefore distinguish resident, home,
and far regions, and the toolchain can distribute code across banks. Mouse
integration made KonaCommander and KonaEdit large enough to require placement.
The initial KonaEdit placement then exposed the xmem capacity problem above.
The SDK's placement rules also had to account for an indirect-call member in
the TUI mouse path and correctly quote `PLACE=` pins.

Evidence: [placement architecture](architecture.md#72-placement-resident-home-and-far),
[application mouse integration](validation/2026-11-01-apps-mouse.md),
[TUI mouse validation](validation/2026-11-01-tui-mouse.md).

**Inference / guidance.** A successful link does not prove that a callback,
interrupt handler, constant, or helper is reachable under every mapping.
When introducing far calls or placement, trace the entire call graph and all
data reads made while the aperture is mapped. Include indirect calls and
interrupt-time code. Inspect the placement report and exercise the code with
the mapping active; do not rely on source-file boundaries as a placement
guarantee.

## 4. Treat hardware state as persistent across software restart

**Observed.** A warm reboot does not reset every peripheral. The M11 device
release/reboot work added explicit release/acquire and console-reset behavior
for devices whose state survives a program or OS transition. Device guidance
requires drivers to reinitialize hardware state. Graphics and pointer work
also found visible state that had to be reset or hidden on release.

Evidence: [device release and reboot](validation/2026-10-13-m11-device-release-reboot.md),
[driver device takeovers](devices.md#14-device-takeovers-and-state-restoration),
[pointer behavior](validation/2026-11-01-tui-mouse.md).

**Inference / guidance.** Do not equate a software reset with a power-on
reset. Specify which owner restores each peripheral, display mode, interrupt
source, queue, and exclusive resource. Test repeated program launch/exit and
warm reboot, including failure after a partial takeover. Cleanup must restore
the device even when initialization or an operation fails midway.

## 5. A measurement can change the behavior being measured

**Observed.** The M13 read-path instrument initially inserted timestamp
operations into a timing-sensitive bulk receive. On hardware, those reads
cost more than the three-byte FIFO coverage, causing bulk transfers to fail
while the command lane continued working. The instrumentation was redesigned
to use two raw counter reads inside the receive path. In a separate graphics
effort, a harness reported CPU instructions as T-states until its clock model
was corrected.

Evidence: [read-path measurement and hardware finding](validation/2026-10-16-m13-read-path-measure.md),
[graphics timing harness correction](validation/2026-10-25-gfx-timing.md).

**Inference / guidance.** Validate the measuring tool before trusting the
numbers. Account for probe overhead, clock units, counter wrap, interrupt
activity, and whether instrumentation changes timing or protocol behavior.
Cross-check with an independent timer or board observation. Keep diagnostic
instrumentation out of production paths unless its cost and semantics are
explicitly acceptable.

## 6. Profile the full path before optimizing a visible bottleneck

**Observed.** Read-path timing was split into transport, cache, staging, and
caller-copy phases before changes were selected. The measurements led to
changes at multiple layers, including read-ahead behavior and a firmware
read-run operation. Console work used per-layer T-state attribution. Shell
directory listing was found to be quadratic because it repeatedly enumerated
entries; one-pass listing removed the repeated work. Conversely, the direct
text-renderer spike measured alternatives before choosing a renderer.

Evidence: [read-path measurement](validation/2026-10-16-m13-read-path-measure.md),
[read-path changes](validation/2026-10-16-m13-read-path-host.md),
[console cost profile](validation/2026-10-21-console-cost-profile.md),
[directory listing](validation/2026-10-16-m13-listing.md),
[direct-text renderer spike](validation/2026-10-21-direct-text-spike.md).

**Inference / guidance.** End-to-end latency can be dominated by a layer that
looks incidental in source. Measure the complete user operation, attribute
time to stages, and compare plausible alternatives on representative data.
After optimizing, repeat the same measurement with the same method and retain
functional equivalence checks. A fast inner loop does not fix repeated calls,
extra copies, or an unfavorable algorithm around it.

## 7. Device command completion and clocks need realistic models

**Observed.** Graphics validation found that the V9958 command engine is
asynchronous: the driver must wait before starting a command, while allowing
the CPU to run after issuing it. Board testing found visual defects that
simulator-oriented work had not made clear, including ellipse gaps and held
screens. The timing harness also needed a device clock model tied to the
board's actual CPU-to-VDP ratio.

Evidence: [graphics pages and async commands](validation/2026-10-25-gfx-pages.md),
[board-found graphics fixes](validation/2026-10-25-gfx-board-fixes.md),
[timing model](validation/2026-10-25-gfx-timing.md).

**Inference / guidance.** Model the device's completion semantics, clock
ratio, and visible state, not just register writes. Distinguish “command
accepted” from “operation finished.” Use board checks for properties the
model cannot prove, especially analog output, timing margins, and visual
quality. Keep the simulator's omissions explicit so a replay is not mistaken
for broader hardware proof.

## 8. Streaming and ring-buffer code must handle boundaries as ordinary cases

**Observed.** The sound ZVG decoder had a ring-wrap defect: a multi-byte
command could read past the end of the 256-byte ring instead of wrapping to
its beginning. Board testing found stray beeps and missed beats. The sound
engine later used an order-buffer streaming design, and ZTR playback checks
covered chronological ordering and CRC behavior. A separate ZTR CRC report
also caught a stale test pin and an incremental image hash that did not match
the final source baseline.

Evidence: [ZVG ring-wrap correction](validation/2026-10-25-zvg-ring-wrap.md),
[ZTR order streaming](validation/2026-10-27-snd-ztr-orders.md),
[ZTR CRC and source identity](validation/2026-10-29-snd-ztr-crc.md).

**Inference / guidance.** For circular or chunked data, test a command split
across the physical end, the smallest and largest legal records, truncated
input, and producer/consumer timing. Use a wrap-aware accessor rather than
assuming contiguous bytes. Compare output against an independent behavioral
oracle where practical. Tie every performance or binary result to a clean
source identity; an incremental build can preserve a stale artifact.

## 9. Validate the user's complete workflow, not only each API

**Observed.** The mouse driver and TUI support passed their component work,
but application integration uncovered that File > Open could not replace a
document because the system-wide object limit was reached. The same application
record notes that opening a 32 KiB file took about two minutes on the board;
simulator timing showed the load cost already existed before mouse support and
grew worse than linearly for the tested inputs.

Evidence: [application mouse integration and load-time data](validation/2026-11-01-apps-mouse.md),
[mouse driver](validation/2026-10-31-hid-mouse.md),
[xmem limit fix](validation/2026-11-01-xmem-objects.md).

**Inference / guidance.** Component tests cannot expose all interactions in
real workflows. Test sequences that keep resources live while opening,
replacing, saving, exiting, and relaunching. Measure realistic input sizes and
record latency as well as correctness. When an old performance defect is
outside the feature's scope, preserve the finding and give it an owner rather
than treating it as solved by nearby work.

## 10. Hardware acceptance must remain separate from simulation

**Observed.** The project records hardware acceptance separately from host and
simulator results. Some recent mouse and xmem records explicitly leave final
board steps pending. Other board runs found issues not visible in simulation,
including read-path instrumentation overhead and graphics artifacts.

Evidence: [M10 hardware checklist](validation/2026-10-11-m10-hardware-acceptance.md),
[read-path board finding](validation/2026-10-16-m13-read-path-measure.md),
[graphics board fixes](validation/2026-10-25-gfx-board-fixes.md),
[current mouse application status](validation/2026-11-01-apps-mouse.md).

**Inference / guidance.** State exactly what each layer proves. A simulator
can establish deterministic software outcomes and explore boundary cases; it
cannot prove electrical behavior, actual interrupt timing, controller
firmware, or display quality unless those behaviors are faithfully modeled.
Keep unresolved board checks marked pending with the image identity, steps,
expected result, and unverified property.

## Technical lessons to carry into future work

1. Measure linked placement, runtime data, stack, and headroom before adding a
   feature to a fixed memory budget.
2. Treat a capacity increase as a cross-layer contract change; audit every
   representation for truncation and exercise the upper boundary and reuse.
3. Trace the full call and data path across mappings, including callbacks and
   interrupts; inspect placement output and test while the mapping is active.
4. Reinitialize persistent hardware state after warm restart and unwind
   partial device takeovers on every error path.
5. Prove that instrumentation measures the intended unit without breaking the
   timing-sensitive path.
6. Profile end-to-end operations, then optimize the measured layer or
   algorithm and rerun the same measurement.
7. Model asynchronous device completion and realistic clock ratios; use board
   checks for properties the model cannot establish.
8. Make circular-buffer wrap, truncated records, resource exhaustion, and
   repeated application workflows ordinary test cases.
9. Keep simulator, board, and source/artifact identity claims distinct.

## Source index

- [Current architecture](architecture.md), [memory map](memory.md),
  [device protocols and ownership](devices.md), [SDK](sdk.md)
- [M9 bank-7 budget](validation/2026-10-04-m9-bank7-budget.md)
- [M10 design spike](validation/2026-10-06-m10-design-spike.md) and
  [two-bank feasibility](validation/2026-10-07-m10-two-bank-feasibility.md)
- [M13 read-path instrumentation](validation/2026-10-16-m13-read-path-measure.md)
- [console profiling](validation/2026-10-21-console-cost-profile.md) and
  [directory-listing optimization](validation/2026-10-16-m13-listing.md)
- [graphics timing and board findings](validation/2026-10-25-gfx-timing.md),
  [graphics board fixes](validation/2026-10-25-gfx-board-fixes.md)
- [sound ring wrap](validation/2026-10-25-zvg-ring-wrap.md) and
  [ZTR CRC](validation/2026-10-29-snd-ztr-crc.md)
- [mouse application integration](validation/2026-11-01-apps-mouse.md),
  [xmem capacity](validation/2026-11-01-xmem-objects.md)
