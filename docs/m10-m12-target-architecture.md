# ZephyrOS M10-M12 Target Architecture

**Status:** Approved target architecture for post-M9 native ZephyrOS work  
**Scope:** M10 through M12  
**Target:** Zephyr-80  
**Relationship to `docs/architecture.md`:** This document supersedes the M10-M12 sequencing and design currently summarized in section 35 of `docs/architecture.md`. It is kept separate while M9/IOC-FAT implementation and size optimization are still in progress. Once M9 closes, these sections should be folded into the main architecture document and section numbering normalized.

---

## 1. Native completion sequence after M9

The native sequence after M9 is:

```text
M10  Extended memory / far code / far data
     + ZEX v2

M11  Complete native diagnostic suite

M12  Native shell and core built-in userland

---------------- native ZephyrOS complete ----------------

Later compatibility/personality architecture
```

The ordering is deliberate:

1. M10 completes native kernel/executable mechanisms.
2. M11 validates and exercises the completed native mechanisms and hardware.
3. M12 builds the normal interactive environment entirely on those completed mechanisms.
4. Compatibility personalities begin only after native ZephyrOS is complete.

M9 already supplies the process-startup and argument-tail contract required by the later shell. Package 28b establishes startup block v1 and synchronous `SYS_EXEC` argument transport; M10 does not redesign that contract.

M9 acceptance and the section-32.39 residency budget remain binding. M10 does not weaken the production arena floor or redefine M9 acceptance to make later work fit. If the measured, optimized end-of-M9 image requires a larger kernel-residency architecture, that decision is made explicitly before M10 implementation rather than hidden inside the far-memory work.

---

# 2. Milestone M10 — Extended memory, far code/data, and native execution completion

## 2.1 Objective

M10 completes the native ZephyrOS memory and executable mechanisms required by applications before the diagnostic suite and native shell are built.

Its responsibilities are:

```text
extended/far memory
    process-owned auxiliary memory objects
    allocations smaller than a physical 64-KiB bank
    bounded copy access to far data

extended native executables
    ZEX v2
    auxiliary segments
    relocatable far code/data
    executable-entry metadata

far execution
    synchronous far calls
    safe bank transition and return
    ordinary native syscalls while far code runs
```

Process startup, argument-tail transfer, nested `SYS_EXEC`, CWD inheritance, descriptor cleanup and parent restoration are already established before M10 and are preserved.

After M10, later native milestones must not require another fundamental memory, executable-format, or process-lifecycle mechanism merely to implement diagnostics or the shell.

The central rules are:

> **Physical banks are a Zephyr-80 implementation detail. Native applications consume memory objects, segments, and handles.**

> **The primary application bank remains the ordinary execution environment. Far memory extends it; it does not replace it.**

> **Executable auxiliary memory is created only by the validated ZEX loader. Runtime far-memory allocation is data-only.**

---

## 2.2 Existing primary-bank model remains

Every native process continues to own one primary application bank.

Normal execution remains:

```text
0000h-00FFh   native page zero
0100h-....    primary ZEX image/data
....          free primary memory
....-DFFFh    primary stack
E000h-FFFFh   permanent common memory
```

Mode `10` remains the normal application view and mode `11` remains the kernel view.

A ZEX that needs no auxiliary segments still behaves like the existing single-bank native program from the application's perspective.

M10 does not turn ordinary native programs into bank-aware programs.

---

## 2.3 Auxiliary-bank geometry

Application banks remain physical SRAM banks `0-6`. Bank 7 remains kernel-owned unless a separately approved later kernel-residency architecture explicitly changes that ownership model.

A bank used as an application auxiliary bank has a different software ownership layout from a primary process bank:

```text
0000h-00FFh   ZephyrOS-owned page zero
0100h-1FFFh   auxiliary execution/control window
2000h-3FFFh   allocation slot 0
4000h-5FFFh   allocation slot 1
6000h-7FFFh   allocation slot 2
8000h-9FFFh   allocation slot 3
A000h-BFFFh   allocation slot 4
C000h-DFFFh   allocation slot 5
E000h-FFFFh   permanent common memory
```

Thus one auxiliary bank provides:

```text
6 x 8 KiB = 48 KiB
```

of allocatable far storage.

The low 8 KiB is deliberately not allocatable. It preserves the same architectural property used by ordinary syscalls: when mode `11` is entered, `0000h-1FFFh` remains visible from the currently selected bank.

This area holds the auxiliary page zero, runtime scratch, far-call frame and auxiliary execution stack.

The exact subdivision inside `0100h-1FFFh` is single-sourced in the platform memory description and validated by the linker/build tools.

---

## 2.4 Allocation granularity is not part of the native ABI

The Zephyr-80 allocator uses 8-KiB slots because they fit the machine's physical memory geometry.

Applications do not request:

```text
bank 5, slot 2
```

They request:

```text
memory object of N bytes
```

and receive an opaque handle.

The kernel rounds the request to the platform allocation granularity.

For example:

```text
native request          Zephyr-80 implementation
------------------------------------------------
allocate 6000 bytes  -> one 8-KiB slot
allocate 12000 bytes -> two contiguous 8-KiB slots
```

A single Zephyr-80 far object must fit in one auxiliary bank and therefore cannot exceed 48 KiB.

A program requiring more storage allocates multiple objects.

This restriction is platform-specific and must not be encoded into the portable meaning of the native memory API.

---

## 2.5 Unified ownership of application banks

The kernel owns one allocator for application banks.

A physical application-capable bank may be:

```text
FREE
PRIMARY(process)
AUXILIARY
```

A primary process bank owns the complete bank and cannot contain auxiliary allocations.

An auxiliary bank may contain multiple 8-KiB allocations.

Allocation policy should preserve whole banks for future `SYS_EXEC` operations. On Zephyr-80 the intended policy is:

```text
primary process allocation: low free bank first
auxiliary allocation:       high bank first, pack existing auxiliary banks first
```

This minimizes fragmentation without making placement observable through the native API.

A bank containing even one live auxiliary allocation is not eligible to become a primary process bank.

Consequently `SYS_EXEC` may return `ZOS_ENOSPC` even when free auxiliary slots exist if no completely free primary bank remains.

That is a valid resource-exhaustion condition.

---

## 2.6 Process ownership and lifetime

Every auxiliary allocation has exactly one owning process.

Ownership applies equally to:

```text
runtime far-data objects
ZEX auxiliary segments
far-code segments
loader-created BSS
```

When a process exits, ZephyrOS releases all auxiliary resources owned by that process.

A suspended parent retains its auxiliary resources while a child runs.

A child does not inherit the parent's auxiliary allocations.

The child receives only:

```text
its primary bank
its own ZEX auxiliary segments
its own later runtime allocations
its inherited CWD and normal process-startup state
```

Failed load, failed relocation, failed far-segment allocation, malformed ZEX metadata or failed launch must roll back every allocation made for the attempted child.

No partial process may become visible.

---

## 2.7 Native extended-memory syscall family

M10 should use the currently available native syscall space for a compact extended-memory family. Exact numbers are pinned when the ABI is implemented.

The semantic operations are:

```text
SYS_XMEM_ALLOC
SYS_XMEM_FREE
SYS_XMEM_READ
SYS_XMEM_WRITE
SYS_XMEM_INFO
SYS_ZEX_SEGMENT_INFO
SYS_FAR_CALL
```

### `SYS_XMEM_ALLOC`

Semantics:

```text
request:
    size
    flags

result:
    opaque handle
```

Runtime allocations are:

```text
process-owned
zero-initialized
read/write
non-executable
```

An allocation of zero bytes is invalid.

### `SYS_XMEM_FREE`

Releases a runtime allocation owned by the calling process.

Loader-created ZEX segments cannot be freed individually.

Invalid, stale or foreign handles are rejected.

### `SYS_XMEM_READ` / `SYS_XMEM_WRITE`

Transfer between:

```text
the calling execution bank
        and
the named far-memory object
```

using:

```text
handle
object offset
caller buffer
length
```

All range arithmetic is checked before any copy occurs.

The caller never maps the object's physical bank directly through this API.

### `SYS_XMEM_INFO`

Returns a versioned logical description:

```text
requested size
allocated size
flags
object type
```

Normal applications are not told the physical bank or slot.

Physical placement belongs only to diagnostics.

### `SYS_ZEX_SEGMENT_INFO`

Returns the logical state of one loader-created auxiliary segment identified by its ZEX segment ID.

It exposes semantic information such as:

```text
segment size
segment flags
loaded/present state
```

but not a writable physical mapping.

### `SYS_FAR_CALL`

Invokes a declared far-code entry point synchronously.

Section 2.13 defines the execution contract.

---

## 2.8 ZEX v2 replaces ZEX v1

M10 replaces ZEX v1 with ZEX v2.

There is no production compatibility path for ZEX v1.

ZephyrOS is still under construction, and executable formats from earlier milestones carry no backward-compatibility obligation. All first-party native programs, tools and tests are converted to ZEX v2 as part of M10.

The ZEX-v1 parser, packer path and format-specific compatibility code are removed once the ZEX-v2 toolchain and loader are accepted.

A ZEX v2 executable contains:

```text
header

primary image descriptor
auxiliary segment table
far-entry table
relocation tables

primary image payload
auxiliary segment payloads
```

A program with no auxiliary segments is simply the minimal form of ZEX v2.

Its primary image continues to load at logical `0100h`.

---

## 2.9 ZEX v2 segment model

The primary segment preserves the current normal process model.

Auxiliary segments are independently described.

Each auxiliary segment contains semantically:

```text
segment id
flags
image size
BSS size
required allocation size
payload location
relocation-table location/count
```

Useful flags include at least:

```text
EXEC
WRITE
```

The flags describe intended use; the Z80 has no hardware protection enforcing them.

An auxiliary segment may contain:

```text
code
constant data
initialized writable data
BSS
```

that must be directly mutually addressable while that segment is selected.

Code and its private static data should therefore normally be packaged into one segment rather than creating cross-bank near references.

An auxiliary segment must fit entirely inside the allocatable `2000h-DFFFh` portion of one auxiliary bank.

---

## 2.10 ZEX v2 uses 32-bit file geometry

The complete ZEX file may exceed 64 KiB once auxiliary segments exist.

Therefore:

```text
file offsets
payload offsets
table offsets
total file size
```

must be validated using 32-bit arithmetic.

The ZEX loader consumes the M9 VFS directly through its 32-bit file-position interface.

It must not inherit a 16-bit source-size limitation from the ZMOD loader.

Individual Zephyr-80 segment sizes remain bounded by the target address-space rules even though the container itself may be larger than 64 KiB.

---

## 2.11 Auxiliary-segment relocation

Auxiliary segments are built as relocatable segment images.

M10 initially requires only the relocation form actually needed by generated Z80 code.

The expected first form is equivalent to:

```text
ABS16:
    word_at(segment_base + patch_offset) += segment_base
```

The host packer and target loader both validate:

```text
relocation type
patch bounds
16-bit overflow
target segment
entry-point bounds
payload/table overlap
```

A normal 16-bit relocation may refer only within its own auxiliary segment.

A direct near pointer from one auxiliary segment to another is invalid because the two segments are not guaranteed to be simultaneously mapped.

Cross-segment relationships use:

```text
far-entry IDs
far-memory handles
explicit far-memory operations
```

rather than hidden 16-bit pointers.

The packer rejects unsupported or ambiguous linker relocations rather than guessing.

---

## 2.12 Far-entry table

Executable auxiliary segments export explicit far-call entry points.

A far entry is identified by a stable ZEX-local entry ID and resolves to:

```text
segment id
offset within segment
```

Applications never pass an arbitrary physical address to `SYS_FAR_CALL`.

The loader validates every declared entry point before publishing the process.

This preserves the distinction used elsewhere in ZephyrOS:

```text
container metadata describes intent
kernel validates placement
application consumes a logical identifier
```

---

## 2.13 Far-call execution contract

`SYS_FAR_CALL` is synchronous.

Conceptually:

```text
primary process
    |
    | SYS_FAR_CALL(entry, frame)
    v
kernel validates entry and ownership
    |
    | copy bounded call frame
    v
auxiliary low-window call frame
    |
    | select target auxiliary bank
    | mode 10
    | auxiliary SP in 0100h-1FFFh
    v
far entry in 2000h-DFFFh
    |
    | RET to common return trap
    v
restore previous execution bank/SP
    |
    | copy returned call frame back
    v
SYS_FAR_CALL returns
```

The stable far-entry ABI is not the SDCC C calling convention.

The kernel-level contract passes a bounded in/out call frame to the far entry. First-party C tooling may generate near-call wrappers that marshal an ordinary C function's parameters into that frame and adapt the result back to SDCC conventions.

Those wrappers are compiler/toolchain implementation, not ZephyrOS ABI.

The call-frame maximum is a build-time architectural constant and must be measured against the auxiliary low-window and stack requirements rather than chosen as an unbounded convenience.

---

## 2.14 Far code uses an auxiliary low window

Before executing far code, ZephyrOS prepares the selected auxiliary bank's low window with at least:

```text
native page zero
RST 30h syscall vector
far-runtime scratch
far-call frame
far execution stack
```

The far function executes in mode `10`.

Therefore it may issue an ordinary:

```text
RST 30h
```

and the existing mode-11 transition preserves that auxiliary bank's `0000h-1FFFh` exactly as it preserves the primary process bank's low window.

The syscall gate must distinguish:

```text
process identity
```

from:

```text
currently selected execution bank
```

after M10.

The owning process remains the same while a far function executes.

User-buffer copies for a syscall use the saved current execution bank, not blindly the process's primary bank.

This is an explicit M10 audit requirement for every existing native syscall family.

---

## 2.15 Restrictions while a far call is active

M10 supports one cross-bank far call at a time per process.

Near calls inside the selected auxiliary segment are unrestricted.

Recursive cross-bank `SYS_FAR_CALL` is rejected with `ZOS_EBUSY`.

`SYS_EXEC` from far execution is also rejected. Process creation originates from the primary execution context.

`SYS_EXIT` remains valid and terminates the complete process regardless of whether it is invoked from primary or far execution. Cleanup releases:

```text
primary bank
all auxiliary segments
all runtime xmem objects
VFS descriptors
other process-owned resources
```

and restores the suspended parent normally.

Later milestones may add recursive far calls only if a real consumer requires them.

---

## 2.16 Existing process-startup contract

M10 preserves the process-startup and argument-tail contract already implemented before this milestone.

`SYS_EXEC` already transports the caller's bounded raw tail into the child's startup block v1 at `0080h`; the kernel does not parse shell syntax.

M10 does not introduce:

```text
a second argv/tail representation
far-specific startup semantics
a new shell-oriented process startup structure
```

Far execution remains part of the same process and therefore does not create a new process-startup environment.

The first-party user runtime remains responsible for interpreting the startup block for C programs.

The process-startup ABI is only changed during M10 if the far-memory implementation exposes a concrete architectural incompatibility. Such a change is treated as a correction to the still-unfrozen native ABI, not as a new M10 feature.

---

## 2.17 M10 portability boundary

The following are Zephyr-80 implementation details:

```text
physical bank numbers
8-KiB slots
mode 10 / mode 11
RST 30h
auxiliary low-window geometry
```

The following are native ZephyrOS concepts:

```text
process
primary address space
auxiliary memory object
segment
far entry
opaque memory handle
logical size
```

No public extended-memory ABI should require a future ZephyrOS port to emulate an 8-KiB Z80 bank slot.

---

## 2.18 Memory-budget discipline

M10 inherits the M9 rule that resident bytes are an architectural resource.

Every package reports the existing bank-7 ledger and common-memory ledger.

Neither the production arena floor nor the common-memory ceiling may be weakened to make M10 fit.

If the optimized end-of-M9 image does not leave enough residency for the M10 kernel mechanisms, kernel residency is expanded through a separately reviewed architecture change before the far-memory implementation proceeds. M10 must not hide such a change inside ordinary feature work.

---

## 2.19 M10 non-goals

M10 does not add:

```text
preemptive scheduling
multitasking
shared writable memory between processes
runtime-generated executable memory
dynamic libraries
arbitrary cross-segment near pointers
recursive cross-bank far calls
virtual memory
swap
memory protection
```

The Z80 remains a cooperative, single-running-process machine.

---

# 3. M10 acceptance criteria

M10 acceptance includes an ordinary ZEX v2 program exercising both the unchanged primary-program case and auxiliary facilities.

At minimum:

```text
ordinary ZEX v2:
    zero auxiliary segments
    normal exec/exit
    nested parent/child exec
    existing startup-tail transport remains intact

xmem allocation:
    1 byte
    8191 bytes
    8192 bytes
    8193 bytes
    multiple objects in one auxiliary bank
    48-KiB maximum object
    oversized rejection
    exhaustion
    fragmentation
    stale/foreign handle rejection
    zero-filled allocation
    automatic cleanup on exit

xmem transfers:
    first/last byte
    slot boundary
    complete object
    out-of-range read/write rejection
    caller-buffer bounds
    exact current-bank restoration

ZEX auxiliary segments:
    one segment
    multiple segments
    image + BSS
    valid ABS16 relocations
    malformed relocation
    relocation overflow
    invalid entry
    segment allocation rollback
    container larger than 64 KiB

far calls:
    primary -> far -> primary
    in/out call frame
    far private BSS
    normal near calls inside far segment
    ordinary syscall from far code
    filesystem syscall from far code
    exact primary SP/bank/IFF restoration
    recursive far call -> EBUSY
    SYS_EXEC from far context rejected
    SYS_EXIT from far context cleans the whole process

lifetime:
    suspended parent retains far allocations
    child receives no parent far handles
    repeated exec/exit leaks no slots
    repeated far calls leak no state

budget:
    production residency constraints remain satisfied
    common linked content remains below its architectural ceiling
    bank7 and common ledgers are emitted
```

Physical hardware is authoritative for bank switching, interrupt interaction and far-call return behavior.

---

# 4. Milestone M11 — Native diagnostic suite

## 4.1 Objective

M11 builds the complete native ZephyrOS diagnostic environment on top of the now-complete kernel mechanisms.

M11 is primarily userland.

It does not move diagnostic implementations into the kernel merely because they inspect hardware.

The existing M7 monitor remains the minimal recovery tool; M11 adds a broader diagnostic suite beside it.

---

## 4.2 Diagnostic layering

The diagnostic suite follows this order of preference:

```text
1. ordinary native service
2. public device/filesystem behavior
3. M7 diagnostic syscall
4. narrowly added diagnostic introspection service
```

A diagnostic must not link private kernel or driver symbols.

Direct memory and I/O operations continue to cross the explicit M7 diagnostic boundary.

The suite does not acquire arbitrary bank-7 read/write access.

---

## 4.3 Diagnostic program organization

The primary diagnostic program is:

```text
/SYSTEM/diag.zex
```

`diag.zex` is an ordinary ZEX v2 executable.

It may use M10 auxiliary code segments to keep the primary image small and to make the diagnostic suite the first substantial production consumer of far-code support.

Conceptually:

```text
primary segment:
    command/menu
    common formatting
    result aggregation

far segments:
    memory/banking
    IRQ/CTC
    serial
    video
    HID
    IOC
    storage
    sound
```

The exact segmentation is a build decision, not a user ABI.

---

## 4.4 Diagnostic command families

The complete suite covers at least:

```text
diag system
diag memory
diag irq
diag serial
diag video
diag hid
diag ioc
diag storage
diag sound
diag all
```

### `system`

Reports and validates:

```text
kernel/native ABI identity
process identity
memory geometry
free primary banks
auxiliary-memory state
loaded modules
registered devices
mounted filesystems
console bindings
```

### `memory`

Tests:

```text
xmem allocation/free
pattern write/readback
slot-boundary transfers
bank restoration
allocator exhaustion/cleanup
```

Destructive arbitrary-bank testing is not the default.

### `irq`

Inspects or tests:

```text
IRQ executive state
source ownership
CTC delivery
unexpected-source accounting
interrupt enable/restore behavior
```

### `serial`

Exercises the registered serial device where practical without replacing the permanent panic/early-console path.

### `video`

Provides:

```text
console test
character/font test
VRAM/data-path test where exposed safely
visual test patterns
```

### `hid`

Provides an interactive keyboard/input test and reports translated events through the normal HID/console path.

### `ioc`

Exercises:

```text
link status
command/reply transactions
bulk read/write path
error/recovery counters where exposed
```

without bypassing the active IOC driver and corrupting its transport state.

### `storage`

Exercises the native filesystem and, where appropriate, block layer:

```text
sequential read/write
seek
metadata
cache behavior
throughput
confirmed write
media/reset behavior
```

A destructive raw-media test requires an explicit destructive mode and never runs under `diag all`.

### `sound`

Exercises the installed sound hardware/driver when present.

Where no native sound service exists yet, a board-level diagnostic may use the explicit diagnostic-I/O boundary rather than turning sound testing into a new kernel subsystem.

---

## 4.5 Read-only diagnostic introspection

M11 may add a read-only diagnostic enumeration service after the existing M7 diagnostic family when concrete tests need information that normal APIs cannot expose safely.

Conceptually:

```text
SYS_DIAG_ENUM

kind
index
output record
output size
```

with versioned record kinds such as:

```text
memory
irq source
device
module
mount
```

The service copies stable descriptive values.

It does not expose:

```text
kernel pointers
driver state pointers
mutable registry objects
bank-7 addresses usable as capabilities
```

End-of-list is distinct from failure.

No generic "call arbitrary driver operation" diagnostic syscall is introduced.

If a diagnostic cannot be expressed safely through normal services, existing diagnostic primitives or this read-only introspection model, that requirement returns to architecture review.

---

## 4.6 Non-destructive by default

Every M11 diagnostic is non-destructive by default.

Potentially destructive operations require an explicit operator choice identifying the resource to modify.

`diag all` must be safe to run on a normal booted system.

Device absence is:

```text
SKIP
```

not failure.

Unsupported optional functionality is also `SKIP`.

A real test failure is `FAIL`.

---

## 4.7 Stable result model

Every diagnostic command produces a final status:

```text
PASS
FAIL
SKIP
```

and returns a process exit status consistent with that result.

`diag all` aggregates the individual tests and exits successfully only when no required test failed.

Human-readable detail may precede the summary, but automated acceptance must be able to identify the final result without screen scraping arbitrary prose.

---

## 4.8 Monitor relationship

`monitor.zex` remains small, independent and recovery-oriented.

M11 does not absorb the full diagnostic suite into the monitor.

The monitor remains usable if:

```text
diag.zex is missing
an auxiliary allocation fails
a diagnostic far segment is malformed
external storage is absent
```

Before M12 supplies the normal shell, acceptance images may launch `diag.zex` directly.

A minimal monitor command that executes a named native program through the ordinary `SYS_EXEC` path may be added for operator convenience, but it creates no new privileged execution path.

---

## 4.9 M11 non-goals

M11 does not add:

```text
a second driver stack
private diagnostic driver entry points
kernel-resident test suites
automatic destructive media testing
a shell
CP/M diagnostics
personality-specific diagnostics
```

It diagnoses native ZephyrOS and the physical Zephyr-80 platform.

---

# 5. M11 acceptance criteria

Acceptance includes:

```text
system:
    coherent version/process/memory report
    device enumeration
    module enumeration
    mount enumeration

memory:
    xmem pattern test
    allocation exhaustion and recovery
    no resource leakage

irq/ctc:
    known interrupt delivery
    counters/state
    exact return state

serial:
    registered path
    bounded failure behavior

video:
    console output
    operator-visible test pattern

hid:
    interactive key/input validation

ioc:
    command exchange
    bulk path
    recovery/error handling

storage:
    filesystem read/write/seek
    cache path
    throughput reporting
    safe failure with no media

sound:
    positive hardware test when installed
    SKIP when absent

aggregate:
    diag all never performs destructive testing
    PASS/FAIL/SKIP aggregation
    meaningful exit status

architecture:
    no private kernel/driver links in diag.zex
    diagnostic ABI exposes no kernel pointers
    monitor still works independently
    ZEX far segments used successfully where configured

budget:
    production residency ledgers pass
```

Hardware-observable device tests require physical acceptance.

---

# 6. Milestone M12 — Native shell and core userland

## 6.1 Objective

M12 provides the normal interactive native ZephyrOS environment.

At the beginning of M12 the kernel, memory system, process model, filesystem, device stack, diagnostics and executable model are already complete.

M12 is therefore a userland milestone.

The shell must not require a new kernel mechanism merely to become usable.

If a fundamental kernel facility is found missing, the omission is treated as an earlier architecture defect rather than hidden inside shell-specific kernel code.

---

## 6.2 Shell is an ordinary process

The native shell is:

```text
/SYSTEM/shell.zex
```

and is an ordinary ZEX v2 application.

It has no private kernel imports.

It uses only:

```text
native console ABI
native filesystem ABI
SYS_EXEC / process ABI
existing startup-tail contract
```

The shell becomes the normal parent process for launched native programs.

The existing lifecycle remains:

```text
shell running
    |
    | SYS_EXEC
    v
shell suspended
    |
    v
child running
    |
    | SYS_EXIT
    v
shell resumed with child status
```

No scheduler is required.

---

## 6.3 Boot policy

After normal boot:

```text
IRQ/device/VFS initialization
    ->
console binding
    ->
launch /SYSTEM/shell.zex
```

`/SYSTEM/shell.zex` is part of the recovery filesystem so the native environment remains available without writable SD media.

If the shell cannot be loaded or launched, ZephyrOS falls back to:

```text
/SYSTEM/monitor.zex
```

through the established recovery path.

A normal shell exit may be relaunched by the kernel supervisor.

An abnormal shell failure is reported and may enter the monitor rather than endlessly relaunching a failing shell.

The exact exit-status policy is pinned during implementation and tested.

---

## 6.4 Command parsing belongs entirely to userland

The shell receives the existing raw startup tail and owns command-line parsing.

The kernel never parses shell syntax.

M12 supports at least:

```text
whitespace-separated words
single-quoted strings
double-quoted strings
backslash escaping
```

For an external command, the shell serializes the resulting arguments into the existing bounded `SYS_EXEC` raw-tail contract used by startup block v1.

M12 does not require:

```text
pipes
I/O redirection
background jobs
job control
shell scripting
variable expansion
command substitution
filename glob expansion
```

Those can be added later without changing the kernel process model.

---

## 6.5 Shell built-ins

The native shell deliberately contains the core userland commands as built-ins.

At minimum:

```text
cd
pwd
ls
cp
mv
rm
mkdir
rmdir
cat
stat
echo
path
help
exit
```

These commands are part of the shell executable rather than separate ZEX programs.

This has several advantages on Zephyr-80:

```text
no repeated exec/exit overhead for simple filesystem operations
no duplicated runtime/startup code across tiny utilities
smaller recovery-store footprint
shared parsing and formatting code
shared filesystem buffers
simpler recovery environment
```

Commands such as `cd`, `pwd`, `path` and `exit` necessarily operate on shell-owned state and therefore must be built-ins.

For architectural simplicity and compactness, M12 applies the same model to the remaining core commands.

This packaging choice is deliberate but does not make the commands kernel services. They remain ordinary userland logic inside `shell.zex`.

---

## 6.6 External command lookup

Commands not recognized as shell built-ins are treated as executable program names.

An argument containing `/` is treated as an explicit path.

A bare command name is resolved entirely by shell policy through a bounded shell-local command path.

A suitable initial default is:

```text
.
/BIN
/SYSTEM
```

Missing path components are skipped.

The shell may try the exact name and then the conventional `.zex` suffix.

The kernel has no `PATH` concept and performs no command search.

Changing the shell command path changes only shell state.

---

## 6.7 Core native userland

The core native userland is the shell itself.

The following facilities are built into `shell.zex`:

```text
filesystem navigation
    cd
    pwd

directory/file inspection
    ls
    stat
    cat

filesystem mutation
    cp
    mv
    rm
    mkdir
    rmdir

general shell utilities
    echo
    path
    help
    exit
```

Larger or specialized applications remain ordinary external ZEX programs.

Examples include:

```text
diag.zex
monitor.zex
editors
compilers
language runtimes
games
future personality launchers
```

Thus the distinction is:

```text
small universal operating commands     -> shell built-ins
substantial applications/tools          -> external ZEX programs
```

The exact internal organization of built-ins may use separate source modules, but they link into one `shell.zex`.

---

## 6.8 Userland filesystem policy

Shell built-ins consume only the M9 filesystem ABI.

### `cp`

Uses ordinary:

```text
open
read
write
close/sync
```

with a bounded shell-owned buffer.

### `mv`

First attempts native `rename`.

If the VFS reports:

```text
ZOS_EXDEV
```

for a cross-filesystem move, the shell may implement:

```text
copy
sync/close
unlink source
```

with appropriate failure handling.

The kernel does not acquire a hidden cross-filesystem rename implementation for the benefit of the shell.

### `ls`

Uses:

```text
opendir
readdir
closedir
stat where required
```

and understands native mountpoints exactly as exposed by VFS.

---

## 6.9 Recovery operation without writable storage

With the normal writable root unavailable, the user must still receive a useful system:

```text
/
`-- SYSTEM/
    |-- shell.zex
    |-- monitor.zex
    |-- diag.zex
    `-- essential recovery assets
```

The shell starts normally.

Read-only commands continue to work.

Mutation attempts against `/SYSTEM` report the normal `ZOS_EROFS` behavior.

The shell does not treat absence of the writable root as a fatal condition.

---

## 6.10 Exit-status handling

`SYS_EXEC` returns the child's native exit status to the shell.

The shell stores the most recent status and reports nonzero failures in a consistent form.

Built-ins and external native programs return meaningful native status values rather than always succeeding after printing an error.

No POSIX-specific status encoding is implied.

---

## 6.11 Shell memory rules

The shell is not special with respect to memory.

It may use:

```text
primary memory
ordinary xmem
far code
```

through exactly the M10 interfaces available to every other native ZEX.

Because the shell contains the complete core userland command set, M10 far-code support may be used to partition built-in command families into auxiliary executable segments if the primary image would otherwise become unnecessarily large.

That is an implementation choice, not part of shell command semantics.

---

## 6.12 M12 non-goals

M12 does not require:

```text
multitasking
preemption
pipes
redirection
background jobs
shell scripts
environment variables
dynamic linking
networking
CP/M execution
other compatibility personalities
```

Those are later facilities or personality work.

---

# 7. M12 acceptance criteria

Acceptance covers at least:

```text
boot:
    shell launched from /SYSTEM
    HID/V9958 normal console
    SIO fallback
    shell available with no writable root
    monitor fallback when shell is missing/rejected

parser:
    empty line
    multiple arguments
    single quotes
    double quotes
    escaped characters
    tail-limit rejection/recovery

built-ins:
    cd
    pwd
    ls
    cp
    mv
    rm
    mkdir
    rmdir
    cat
    stat
    echo
    path
    help
    exit

execution:
    bare-name command search
    explicit absolute path
    explicit relative path
    .zex suffix handling
    child exit 0
    child nonzero exit
    malformed executable
    repeated child execution leaks no process/bank/xmem/fd state

CWD:
    cd absolute
    cd relative
    ..
    child inherits shell CWD
    child chdir does not alter shell CWD

filesystem policy:
    mv same filesystem
    mv cross-filesystem through copy/sync/unlink
    writes to /SYSTEM fail cleanly with EROFS

native tools:
    launch monitor
    launch diag
    return cleanly to shell

recovery:
    /SYSTEM visible with SD absent
    read-only built-ins work
    mutation failure is clean and non-fatal

budget:
    final production residency ledgers pass
    recovery ROMFS fits
```

Physical acceptance includes a complete interactive session on the HID/V9958 console and the SIO fallback console.

---

# 8. Native ZephyrOS completion boundary

Completion of M12 marks the first complete native ZephyrOS environment.

At that point the system has:

```text
boot/recovery
memory ownership
interrupt executive
dynamic drivers
native device model
native console
process lifecycle
native executable format
hierarchical VFS
writable filesystem
extended/far memory
far code/data
complete diagnostics
native shell
core built-in userland
```

Only after this boundary does detailed compatibility/personality architecture begin.

Personality implementations consume native ZephyrOS mechanisms rather than forcing compatibility concepts into the native kernel.

Initial Zephyr-80 personality candidates include:

```text
CP/M
Coleco
GameOS
```

Future ZephyrOS ports may define personalities appropriate to their processor/platform family.

The native kernel itself remains free of:

```text
CP/M FCB semantics
Coleco BIOS semantics
foreign filesystem namespaces
foreign process ABIs
```

except at explicit personality boundaries.

---

## 8.1 Native ABI baseline

During M0-M12 ZephyrOS deliberately carries no backward-compatibility obligation between development milestones.

Completion of M12 ends that phase.

The accepted:

```text
native syscall ABI
ZEX v2 format
driver/module ABI
filesystem ABI
process-startup ABI
extended-memory ABI
```

form the first stable native ZephyrOS compatibility baseline.

Subsequent incompatible changes require an explicit ABI or format version change rather than silently changing an established contract.

---

# 9. Milestone summary

```text
M0   Kernel bring-up                         COMPLETE
M1   Kernel foundation                      COMPLETE
M2   Interrupt executive                    COMPLETE
M3   Device registry/static drivers         COMPLETE
M4   Dynamic module loading                 COMPLETE
M5   ROM recovery filesystem                COMPLETE
M6   Native process/ZEX foundation          COMPLETE
M7   Native monitor/diagnostic foundation   COMPLETE
M8   Native device stack                    COMPLETE
M9   Native VFS/filesystem framework        IN PROGRESS
 |
 v
M10  Extended memory / far code/data
     + ZEX v2
 |
 v
M11  Complete native diagnostic suite
 |
 v
M12  Native shell and core built-in userland
 |
 v
---------------- native ZephyrOS complete ----------------
 |
 v
Compatibility/personality architecture
```
