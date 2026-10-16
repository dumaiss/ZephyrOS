# KonaOS Personalities

**Status:** architecture defined; CP/M is the first planned personality

This document defines the execution-personality mechanism and the first
personality, CP/M.  It is subordinate to `docs/architecture.md`; where the
two disagree, the architecture document wins.

A personality is an **alternative execution environment for a KonaOS
process**.  It is not an emulator process, a resident virtual machine, or a
native application that hosts another process.  The shell launches a
personality program directly as its child, and the normal KonaOS process
lifecycle still owns allocation, suspension, exit and cleanup.

The first personality runs ordinary CP/M `.COM` binaries directly on the
Z80.  KonaOS does not emulate the processor and does not translate the
program into C.

---

## 1. Principles

1. **The native system stays native.** CP/M drive letters, USER areas, FCBs,
   128-byte records and page-zero conventions never enter the normal KonaOS
   ABI.
2. **A personality is a process environment, not an application.** There is
   no architectural `cpmrun` process between the shell and the CP/M child.
3. **The shell chooses the personality.** The kernel does not infer it from
   a filename suffix.  A shell may recognize `.EXE` as native and `.COM` as
   CP/M, then pass the selected personality to `SYS_EXEC`.
4. **The Z80 runs the original binary.** A CP/M program executes natively in
   mode `01`; only its operating-system calls cross into KonaOS.
5. **Reuse the native process lifecycle.** A personality child owns one
   process slot and one primary bank, suspends its parent while it runs, and
   exits through the normal process teardown path.
6. **Do not weaken the native syscall ABI.** The CP/M crossing is a private
   personality gate.  Native programs continue to use the documented
   `RST 30h` ABI and its existing argument-window rules.
7. **Keep compatibility policy in the personality.** CP/M semantics are
   implemented by the personality runtime; native services provide files,
   console I/O and other mechanisms underneath it.
8. **The first implementation targets well-behaved BDOS applications.** A
   Z80 `IN` or `OUT` instruction cannot be trapped, so programs that drive
   hardware directly are outside the initial compatibility contract.

---

## 2. Launch model

The production shell launches native and CP/M programs through the same
process mechanism:

```text
                         shell
                           |
                  +--------+--------+
                  |                 |
              native EXE          CP/M COM
                  |                 |
              mode 10            mode 01
                  |                 |
                  +--------+--------+
                           |
                          exit
                           |
                    shell resumes
```

There is no:

```text
shell -> cpmrun -> program
```

The CP/M program is the shell's direct child.

`SYS_EXEC` therefore carries a personality selector in addition to the path
and raw command tail.  The required personalities are conceptually:

```text
KOS_PERSONALITY_NATIVE
KOS_PERSONALITY_CPM
```

The exact public structure and numeric values are defined with the ABI when
the implementation is added.  The selector is explicit: the nucleus does
not test whether a path ends in `.COM`.

The shell owns command-resolution policy.  For example, a command resolver
may try the native command path for `.exe` and then the CP/M command path for
`.com`; an explicit `.COM` path is launched with the CP/M personality.

A native exec follows the existing EXE loader.  A CP/M exec instead:

1. allocates a child process and a CP/M-capable primary bank;
2. resolves and opens the `.COM` file through the native namespace;
3. verifies that the raw image fits below the personality gateway;
4. loads it at `0100h`;
5. installs CP/M page zero, the gateway and the guest personality runtime;
6. constructs the command tail and default FCBs;
7. establishes the initial CP/M drive mapping;
8. suspends the parent only after every fallible preparation step succeeds;
9. enters the child at `0100h` in mode `01` with maskable interrupts off.

Warm boot or BDOS function 0 exits the child.  Normal KonaOS teardown closes
its native handles, releases its bank and resumes the parent exactly as for a
native child.

---

## 3. Bank allocation and flat mode

Mode `01` maps the selected SRAM bank over the complete `0000h-FFFFh`
address space.  There is no common-memory overlay while the guest runs.

A CP/M personality **must not use bank 0 as its primary bank**.  Bank 0
`E000h-FFFFh` is the physical backing of KonaOS common memory.  In mode `01`
over Q = 0 those bytes would be exposed as ordinary guest RAM, and a CP/M
write there would corrupt the common region.

The CP/M loader therefore allocates its primary bank only from banks `1-5`.
This restriction belongs to the personality allocator; it does not change
native process allocation.

With a nonzero primary bank, the complete high 8 KiB is safe guest-private
RAM while mode `01` is active and is used by the CP/M personality runtime.

---

## 4. CP/M guest memory map

The initial CP/M layout is:

| Address | Purpose |
|---|---|
| `0000h` | CP/M warm-boot/exit vector |
| `0005h` | `JP` to the CP/M BDOS entry/gateway |
| `005Ch`, `006Ch` | default FCBs |
| `0080h` | CP/M command tail and initial DMA buffer |
| `0100h-DBFFh` | CP/M TPA: program, overlays, heap and guest stack |
| `DC00h-DFFFh` | personality bridge, saved guest state, private bridge stack and transfer buffers |
| `E000h-FFFFh` | guest CP/M personality runtime and CP/M-private state |

The BDOS entry advertised by page zero is `DC00h`, making `DC00h` the TPA
ceiling seen by software that examines the jump at `0005h`.  A raw `.COM`
image must therefore fit entirely in `0100h-DBFFh` when first loaded.

The region `DC00h-DFFFh` is deliberately chosen because it maps to the same
physical bytes in both relevant mappings:

```text
mode 01, Q = guest bank    DC00h-DFFFh -> guest bank
mode 10, Q = guest bank    DC00h-DFFFh -> guest bank
```

Only `E000h-FFFFh` changes when the bridge switches from mode `01` to mode
`10`.  Therefore the bridge can execute an `OUT` to the memory latch and
continue at the next instruction without a mirrored code trick.

The guest runtime in `E000h-FFFFh` is not visible while native common memory
is mapped.  Every request or result that must survive a native crossing is
therefore staged in the bridge/transfer area below `E000h`.

---

## 5. CP/M page zero

The loader synthesizes CP/M page zero for every launch.  At minimum it
contains:

```text
0000h   JP personality warm-boot/exit bridge
0003h   IOBYTE-compatible state if required
0004h   CP/M current-drive byte where required by compatibility
0005h   JP DC00h                     ; BDOS entry
005Ch   default FCB 1
006Ch   default FCB 2
0080h   command-tail length
0081h   command-tail bytes
```

The command tail is derived from the raw tail passed by the native shell,
using CP/M's length-and-bytes convention rather than the native startup
block.  The personality also parses the first two command-tail file tokens
into the default FCBs using CP/M rules.

`CALL 0005h` is the normal guest operating-system entry.  `JP 0000h` and
BDOS function 0 terminate the personality process rather than loading a CP/M
CCP.

---

## 6. The guest BDOS runtime

Most CP/M semantics belong in the guest personality runtime rather than in
the nucleus.  The high guest region can implement, among other things:

- current drive;
- USER number;
- current DMA address;
- FCB parsing and 8.3 conversion;
- sequential and random record arithmetic;
- wildcard matching state;
- directory-search state;
- drive-to-native-directory mappings;
- console-output buffering;
- translation of CP/M return conventions.

Calls that are pure CP/M state need not enter KonaOS at all.  Examples
include the CP/M version query, selecting/getting the current drive, setting
the DMA address and USER bookkeeping.

A native crossing is made only when the operation requires a native
mechanism such as:

```text
CONSOLE input/output
VFS path resolution or namespace enumeration
FILE bind/read/write/seek/unbind
FS create/delete/rename/stat operations
process exit
```

This keeps compatibility policy out of the native ABI and avoids paying a
bank/kernel crossing for operations that can be answered locally.

---

## 7. Personality bridge and native gate

The CP/M bridge at `DC00h-DFFFh` is the only code that crosses between the
flat guest mapping and the native application mapping.

A native operation follows this shape:

```text
CP/M program, mode 01
        |
        | CALL 0005h
        v
CP/M BDOS runtime
        |
        | native operation needed
        v
DCxx bridge
        |
        | save guest registers and SP
        | move to bridge-private stack
        | stage fixed request/transfer data below E000h
        | switch mode 01 -> mode 10 over the same Q bank
        v
private personality gate
        |
        | enter nucleus/native services
        | VFS / FILE / FS / CONSOLE
        v
DCxx bridge, mode 10
        |
        | switch mode 10 -> mode 01
        | restore guest SP and registers
        v
CP/M BDOS runtime -> RET
```

The bridge uses its own stack.  A CP/M program's stack is guest state and may
be anywhere below the advertised BDOS boundary; native code never relies on
it after the mapping changes.

### 7.1 Not an ordinary syscall

The personality crossing is **not** an ordinary `SYS_CALL` or public
`RST 30h` request.  The native syscall ABI requires its argument block in the
normal low caller window, whereas the personality deliberately stages its
control data in the fixed bridge region near `DC00h`.

The implementation therefore provides a private personality entry with a
fixed request format and fixed transfer-area bounds.  It is callable only by
a process whose descriptor marks it as the matching personality.  It does
not appear in `abi/kos/` and ordinary native programs cannot depend on it.

The entry validates the personality, the fixed bridge bounds and the native
operation before touching the request.  Once accepted, native handles and
objects created on behalf of the guest remain ordinary resources owned by
that KonaOS process and are reclaimed by normal process exit.

---

## 8. Interrupt model

The first CP/M personality runs guest code with maskable interrupts disabled.
This avoids exposing KonaOS IM2/common-memory machinery while mode `01` has
removed the common region.

The bridge transition owns the interrupt state:

1. guest code and the initial bridge run with `DI`;
2. the bridge switches to mode `10`, where KonaOS common memory and interrupt
   state are visible again;
3. the private personality gate may enable interrupts while native work is
   executing, so the normal timer and resident-driver IRQ executive can run;
4. the gate returns to the bridge with interrupts masked;
5. the bridge switches back to mode `01` and returns to the guest still
   masked.

A later, more ambitious CP/M personality may implement a guest interrupt or
BIOS bridge, but it is not required for ordinary BDOS-oriented applications.

---

## 9. Files, drives and FCBs

The personality presents CP/M drives as views of native directories.  The
mandatory initial rule is:

```text
A: -> directory containing the launched .COM file
```

Thus a self-contained application directory such as:

```text
/fs/APPS/SC2/
    SC2.COM
    SC2.OVL
    SC2.HLP
```

naturally appears to the program as files on drive A.  This preserves the
common CP/M assumption that the executable and its overlays/help files live
on the current disk without inventing a new package format.

Additional drive mappings are personality configuration/policy and are not
part of the native filesystem namespace.  An unmapped CP/M drive fails with
the appropriate CP/M result.

The personality translates uppercase CP/M 8.3 names to native paths.  Native
paths remain native; KonaOS does not acquire drive letters or USER areas.

### 9.1 Record I/O

CP/M sequential and random file operations are expressed in 128-byte
records.  The personality converts them to native byte-oriented FILE
operations using seek/read/write as necessary.  Short native reads and EOF
are translated to the CP/M conventions expected by the BDOS function.

### 9.2 Open-file associations

FCB contents are authoritative CP/M state.  The personality may cache an
association from a guest FCB address to a native file handle, but the address
is not itself object identity: a CP/M program may copy an FCB.

Directory Search First/Next is process-level CP/M state and may keep one
personality search cursor plus the information needed to synthesize CP/M
directory entries.

---

## 10. Console behaviour and buffering

Console output is performance-sensitive because many CP/M programs emit a
screen one character at a time, especially through BDOS function 6.  A
native IPC crossing for every character is unacceptable on the Zephyr-80.

The personality therefore keeps a small output buffer in the persistent
bridge/transfer area, initially 128 or 256 bytes:

- ordinary direct-console output appends locally;
- a full buffer is flushed as one native `CONSOLE` write;
- any BDOS operation that is not buffered output flushes first;
- input flushes before waiting for a character;
- exit flushes before process teardown.

The buffer contains bytes, not parsed terminal operations.  ANSI escape
sequences produced by the guest are passed through as ordinary byte runs to
the selected KonaOS console implementation.

BDOS function 5 is the CP/M LIST device rather than ordinary console output.
The initial personality may report it unsupported or route it according to a
separate compatibility policy; it is not silently part of the console path.

Function 6 is dual-purpose.  Only its output form is buffered; its input and
status forms use the corresponding native console operation after first
flushing pending output.

---

## 11. Initial CP/M compatibility surface

The first implementation is driven by real applications rather than by an
attempt to reproduce every CP/M implementation detail at once.  SuperCalc 2
(SC2) is the first acceptance target because it exercises overlays, help,
terminal I/O and writable files while remaining fundamentally BDOS-based.

Static analysis of SC2 identified these relevant BDOS functions:

| Area | Functions |
|---|---|
| Console / LIST | `5`, `6`, `10` |
| System | `12`, `13`, `14`, `25`, `32` |
| File open/close | `15`, `16` |
| Directory search | `17`, `18` |
| File management | `19`, `22`, `23` |
| Sequential I/O | `20` |
| DMA | `26` |
| Random I/O | `33`, `34` |
| Application-specific/unusual | `155`, `159` |

Functions `155` and `159` are traced on hardware before their semantics are
committed.  Returning an appropriate CP/M 2.2 version from function 12 may
avoid version-specific paths in applications, but unknown calls are never
silently invented merely to satisfy one binary.

`SC2.OVL` and `SC2.HLP` remain ordinary separate files.  They are not
appended to or preloaded with `SC2.COM`; SC2 reaches them through the same
FCB/BDOS path as on CP/M.

---

## 12. Hardware access and compatibility boundary

The Zephyr-80 Z80 has no instruction trap for arbitrary I/O access.  A guest
can execute `IN`, `OUT`, `DI`, `EI`, change `I`, or write the memory latch if
it chooses.  KonaOS is already a trusted system, so the personality does not
pretend to provide virtualization or protection that the hardware cannot
enforce.

The supported compatibility class is therefore:

- ordinary CP/M applications that use BDOS for console and file services;
- applications that assume standard page-zero conventions and a normal TPA;
- overlays and support files opened through BDOS.

The initial personality does not promise transparent support for software
that:

- programs a CP/M disk controller directly;
- drives a particular video or serial controller directly;
- depends on a specific CBIOS jump table or interrupt implementation;
- rewrites the Zephyr memory latch or other machine-control ports.

A full machine emulator could trap and virtualize those behaviours, but that
would be a different mechanism with a substantial performance cost and is
not part of this personality design.

---

## 13. Native resources and cleanup

Although the guest sees CP/M, the child remains one KonaOS process.  Native
resources opened through the personality gate are owned by that process in
the existing tables:

```text
process slot
primary bank (1-5 for CP/M)
standard/native console handles as needed
native FILE/FS handles opened on behalf of FCBs
CWD inherited at exec
```

CP/M-private state remains in guest memory wherever possible.  The native
process descriptor needs only enough personality metadata to identify the
execution environment and enforce the private gate.

On warm boot, BDOS 0, or an explicit personality fatal exit, the bridge
returns to a native-safe mapping and enters the normal process-exit path.
That path unbinds every handle owned by the process, frees its bank and any
other native resources, restores the suspended parent and returns the child
status through the normal `SYS_EXEC` result.

No second cleanup mechanism exists for CP/M.

---

## 14. Implementation order

The first implementation should be built in narrow slices:

1. extend exec with an explicit personality selector and teach the shell to
   launch `.COM` paths as CP/M children;
2. add CP/M-only primary-bank allocation from banks `1-5`;
3. load raw `.COM` files at `0100h` and synthesize page zero, command tail
   and default FCBs;
4. install the `DC00h-DFFFh` bridge and the `E000h-FFFFh` guest runtime;
5. implement warm boot/BDOS 0, version, drive/USER and DMA state locally;
6. add the private native personality gate and console input/output;
7. add FCB open/close/search/sequential read over native VFS/FILE services;
8. boot SC2 and verify `SC2.OVL` and `SC2.HLP` through drive A;
9. add create/delete/rename and random record I/O for worksheet saving;
10. trace functions `155` and `159` and implement only the behaviour proved
    necessary;
11. add and benchmark buffered console output;
12. generalize compatibility only after SC2 and a broader CP/M application
    test set are stable.

---

## 15. Non-goals

The CP/M personality does **not** add the following to native KonaOS:

```text
drive letters
USER areas
FCBs
128-byte native file records
CP/M directory sectors
CP/M BIOS calls
CP/M error codes in the native ABI
runtime hardware discovery
multitasking
process isolation
Z80 instruction emulation
```

It also does not require another resident OS bank, another application
process, or a permanent CP/M service driver in bank 6.

The design is deliberately asymmetric: native KonaOS remains the operating
system; CP/M is a compatibility execution environment that consumes native
facilities and disappears completely when its child process exits.
