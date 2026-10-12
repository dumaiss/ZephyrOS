# The KonaOS user SDK

`user/sdk/` is the only supported way to build a native KonaOS program.
A program is a C file with `int main(int argc, char **argv)`, one line in a
Makefile, and nothing else: no entry assembly, no hand-written manifest, no
kernel header, no hosted C library.

```c
#include "kos.h"

int main(int argc, char **argv)
{
	printf("Hello, KonaOS!\r\n");
	return 0;
}
```

```make
$(eval $(call exe_program,hello,user/helloworld,hello.c,STACK=256))
```

Package 28c introduced this layout; the public ABI it compiles against
lives once, in `abi/kos/` (see `docs/abi.md` for the ABI itself and
`docs/exe-format.md` for the container format).  `user/ZShell/` (M10) must
be built from the SDK alone and is the SDK's reference implementation.

## 1. Layout

```text
abi/kos/                  the public native ABI (one copy, all builds)
  types.h errno.h syscall.h fs.h process.h diag.h

user/sdk/
  include/kos.h           the umbrella header a program includes
  include/kos/            console.h fs.h ipc.h process.h diag.h seg.h xmem.h
  include/klibc/          string.h fmt.h stdarg.h
  src/crt0.asm            C startup: startup block, argv, main, SYS_EXIT
  src/runtime.asm         shared argument scratch + error mapper
  src/gate.asm            exit/getinfo gate stubs
  src/fsgate.asm          filesystem gate stub
  src/diaggate.asm        diagnostic gate stubs
  src/sdcc_calls.asm      the indirect-call trampolines the far call emits
  src/args.c              startup(), splitargs()
  src/fs.c                open/read/... wrappers
  src/exec.c              run()
  src/xmem.c              extended-memory and aperture wrappers
  src/seg.c               EXE v2 segment wrappers, farcall
  src/console_proto.c   console-driver client (write/read/poll/putc)
  src/console_io.c        putchar/console_puts/getchar/console_readline
  src/klibc/              one archive member per function
  src/kos_internal.h      SDK-private glue (not for programs)
  exe.mk                  the generic exe_program build rule
  template/hello.c        the copyable minimal program

user/monitor/             monitor program + monitor-private helpers
user/helloworld/          demos (hello.c, fardemo.c) and their Makefile fragment
```

`user/monitor/` keeps only what is specific to the monitor: its command
engine, `hexfmt`, `hexdump` and `status` output.  Anything reusable lives
in the SDK.

## 2. Include rules

A program compiles with exactly:

```text
sdcc -mz80 -c --nostdinc --Werror -I abi -I user/sdk/include -I <program dir>
```

- **`#include "kos.h"`** is the only header a program needs; it pulls in
  `kos/console.h`, `kos/fs.h`, `kos/ipc.h`, `kos/process.h`,
  `kos/seg.h`, `kos/xmem.h`, `klibc/string.h` and `klibc/fmt.h`.  The
  diagnostic wrappers are **not** re-exported (package 46 T6): a program
  that uses them includes the SDK's diag header itself, as the monitor
  does.
- The SDK's `kos/*.h` headers include the matching **`abi/kos/*.h`**
  header, so the ABI declarations exist exactly once.  SDCC resolves a
  quoted include in the including file's own directory first, which is why
  `user/sdk/include/kos/fs.h` gets `abi/kos/fs.h` (and not itself).
- A program may include its own local headers (`monitor.h`,
  `hexdump.h`, ...).  A test program may add a narrow test-owned include
  root with `INCLUDES=`.
- A program must **never** include `kernel/`, `drivers/`, `drivers/include/`,
  `formats/`, `platform/` or `fs/`.  `-I` is not even given
  for them, and `tests/test_sdk.py` resolves every quoted include of every
  declared program to prove it.
- There is no hosted C library: `--nostdinc` plus `klibc` (section 5)
  replace its string, memory and formatting parts.  No `malloc`, no stdio
  streams, no floating point, no POSIX emulation.

## 3. Build rule: `exe_program`

```make
include user/sdk/exe.mk
$(eval $(call exe_program,NAME,DIR,SOURCES[,OPTION=VALUE ...]))
```

`SOURCES` are relative to `DIR`; `.c` files go through SDCC, `.asm` files
through `sdasz80`.  The rule compiles them, links crt0 and the shared
runtime object, then the program objects, then `libkos.lib`, derives the
manifest from the link map, packs and validates
`build/user/NAME.exe`, and keeps every intermediate under
`build/user/NAME/`, so programs can be built in parallel.

| Option | Default | Meaning |
| --- | --- | --- |
| `STACK=N` | `256` | child stack reservation in the manifest |
| `SCRATCH=SYM:SIZE` | `kos_scratch:16` | scratch symbol the packer must find in `0100h-1FFFh`; `SCRATCH=` declares none |
| `ENTRY=SYM` | `kos_entry` | entry symbol (crt0's entry) |
| `NO_CRT0=1` | off | link no crt0; `ENTRY` names the program's own entry (the fixed SDK low-data/runtime object remains) |
| `LOWBASE=ADDR` | `0x0100` | base of the SDK's fixed gate-visible data area (93 bytes: the object/namespace enumeration and exec blocks are part of it) |
| `CODEBASE=ADDR` | `0x0144` | base of executable code, immediately after the ordinary low-data area (`0x0159` in the `KOS_NS_HANDLES` variant) |
| `STAGE=1` | off | copy the packed program into `$(MODULES)` so a recovery manifest can name it |
| `INCLUDES=a:b` | empty | extra include roots (colon separated), for test-owned headers only |
| `SEGMENTS=FILE` | none | a JSON list of EXE v2 segment declarations: `DATA` rows (`id`, `kind`, `input`, `bss_size`) and `FAR_CODE` rows (`id`, `kind`, `area`, `bss_size`); `FILE` is a build prerequisite and each payload `input` resolves against the program's object directory |
| `FAR=SRC:SRC` | none | far-code sources (relative to `DIR`) placed in `_FAR_CODE`, which the link bases at `8000h`; a `.c` source is compiled with `--codeseg FAR_CODE` and an `.asm` source opens `.area _FAR_CODE` itself.  Without `SEGMENTS=` the rule declares the one `FAR_CODE` segment (id 1) for it |

The rule file is a prerequisite of the archive and of every link script,
so editing it (a new source, a changed flag) re-archives `libkos` from
scratch and relinks each program; a stale archive can never keep a removed
member alive.

There is **no hand-written manifest**: `tools/exemanifest.py` computes
`image_size = s__DATA - 0100h`, `bss_size = l__DATA` and
`entry_offset = ENTRY - 0100h` from the link map, and `exepack` validates
the packed container against the map's symbols (including the scratch
window rule).  `SEGMENTS=FILE` merges auxiliary segment declarations;
the packer derives each `DATA` segment's `image_size` from its payload
file and each `FAR_CODE` segment's from the map's `l__<area>` (whose
`s__<area>` must be `8000h`), then lays out the 36-byte v2 header, the
table and the payloads (`docs/exe-format.md`).

A rule line with `FAR=` also switches on the **36.17 link rule**: the
packer proves the primary image and BSS end below `8000h` and the stack
base sits at or above `C000h`, so a program that uses far code keeps
`8000h-BFFFh` for the aperture alone.  `fardemo` (section 10) is the
smallest program that links that way.

## 4. crt0, argv and exit

The kernel's launch gate enters the program at `kos_entry` (crt0, linked
first so it lands at `0100h`) with `HL = 0080h`, the versioned startup
block (`docs/exe-format.md` section 5.1):

1. crt0 stores `HL` in `kos_startup_block`, so `startup()` can hand the
   raw block to a program later (the kernel's page-zero copy is not
   writable through it);
2. it validates `version == 1` and `tail_length <= 125`; anything else is
   treated as an empty command line rather than reading past the block;
3. it copies the tail into the SDK's own 127-byte buffer and calls
   `splitargs(tail, argv, KOS_ARGV_MAX)` over that copy, which splits **in
   place** (the quotes, spaces and NULs are rewritten in program BSS);
   the startup block is an entry-time delivery, not durable storage (it
   lies below the `0100h` syscall buffer window), so `argv` must point
   into program data for the words to survive the syscalls `main()`
   makes;
4. it calls `int main(int argc, char **argv)` with `argc` clamped to
   `KOS_ARGV_MAX` (16) even when more words were counted;
5. on return it calls `exit` (`SYS_EXIT`) with the low 16 bits of the
   return value, and halts if that ever returns.

`argv` holds the command-tail words only: the kernel transports no program
name, so a bare `/system/hello.exe` launch calls `main(0, argv)`.  The
splitter's quoting rules are documented in `docs/exe-format.md` section
5.1 and are runtime policy, not ABI.  A program that ignores its arguments
may write `int main(void)`.

**A clean exit is not terminal: the boot launcher re-execs its configured
program, forever.**  `kernel_boot_launcher()` (`kernel/main.c`) is a
`for (;;)` around `process_exec(boot program)` plus one report line:

```text
monitor: exit status 0x0000 active 0 free-banks 6
```

(the label is the 28b constant, not the program's name).  The loop is
specified behavior inherited from the M7 monitor contract, where `Q` had
to return to a fresh monitor, and 28b kept it when the launcher became
program-agnostic.  Consequences:

- a program that exits (as `hello.c` is required to) is started again
  immediately, so its transcript is a **repeating** greeting/report pair,
  at full console speed, until the board is reset;
- nothing leaks across those cycles: the report's `active` and
  `free-banks` fields are the post-teardown counters, so a long repeating
  transcript with a constant `active 0 free-banks 6` is itself the
  end-to-end evidence that every exit returned the process slot, the
  application bank, the descriptor pool and the CWD to baseline;
- the machine never reaches the kernel's idle loop, so there is no prompt
  to return to in a demo image.  Parking instead of exiting, or halting
  after a normal exit, would both need either a behavior change in the
  launched program or a change to the launcher policy in `kernel/main.c`
  (kernel bytes); 28c deliberately does neither.

## 5. `klibc`

One archive member per function, so a program links only what it calls.
Nothing here allocates.

`klibc/string.h`:

```c
unsigned int strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  strncmp(const char *a, const char *b, unsigned int n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
void *memcpy(void *dst, const void *src, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int c, unsigned int n);
int   memcmp(const void *a, const void *b, unsigned int n);
unsigned int strlcpy(char *dst, const char *src, unsigned int size);
unsigned int strlcat(char *dst, const char *src, unsigned int size);
```

`klibc/fmt.h`:

```c
void utoa(char *dst, unsigned int value, unsigned char base,
              unsigned char width, char pad);
void ultoa(char *dst, unsigned long value, unsigned char base,
               unsigned char width, char pad);
void itoa(char *dst, int value, unsigned char base,
              unsigned char width, char pad);
void ltoa(char *dst, long value, unsigned char base,
              unsigned char width, char pad);
int  printf(const char *fmt, ...);
int  snprintf(char *dst, unsigned int size, const char *fmt, ...);
```

`printf` supports `%s %c %d %u %x %ld %lu %lx %%` with an optional
width and a `0` zero-pad flag (`%05d`, `%4s`); `snprintf` is the same
formatter writing into a bounded buffer that is always NUL-terminated.
`printf` writes through the console driver (`console_write`).

Integer division and modulo are a language feature, not a library
function: the compiler emits calls to `__divuint`, `__moduint`,
`__divulong`, `__modulong`, `__divsint` and friends.  `klibc` provides
them (`sdcc_arith.c`, `sdcc_arith_signed.c` plus the assembly entry
stubs), implemented with shifts and subtraction so `libkos` itself needs
no division.

## 6. libkos API

The ABI's raw syscalls (`SYS_*`, `RST 30h`) are wrapped once, here.  Every
wrapper takes arguments in the documented native convention, keeps its
argument block in the shared low-window scratch, and returns the syscall
result or the negated `errno` (`-KOS_EINVAL`, `-KOS_EFAULT`, ...).

`kos/console.h` (console client: `console_proto.c`; exit/getinfo gate stubs:
`gate.asm`):

```c
void exit(int status);                          /* SYS_EXIT, does not return */
int  console_write(const char *buf, unsigned int len);  /* console driver WRITE */
int  console_read(char *buf, unsigned int len);         /* READ, non-blocking    */
int  console_poll(void);                                /* POLL                  */
int  putchar(int ch);                                   /* PUTC, no descriptor   */
int  getinfo(struct kos_info_record *record);           /* SYS_GETINFO */
int  console_puts(const char *text);
int  getchar(void);                          /* blocking */
int  console_readline(char *buffer, unsigned int capacity);
```

Package 41 removed the `SYS_CONSOLE_*` syscalls: these four entries are
clients of the resident console driver
(`abi/kos/protocol.h`, class `KOS_CLASS_CONSOLE`), resolved once through
`SYS_IPC_LOOKUP` and cached; a program whose store lists no console
driver sees `-KOS_ENOENT`.  `putchar` uses the descriptor-less PUTC
operation, the rest one descriptor-bearing call each.

`console_readline` is bounded line editing with backspace and CR handling
(`KOS_LINE_MAX` is 80); it is the monitor's input loop and any program's
cheap command reader.

`kos/fs.h` (gate stub: `fsgate.asm`, wrappers in `fs.c`):

```c
struct kos_fs_call_req { unsigned char number; void *block; };
int  kos_fs_call(struct kos_fs_call_req *req);   /* the single gate stub */

int  open(const char *path, unsigned char flags);
int  close(unsigned int fd);
int  read(unsigned int fd, void *buffer, unsigned int length);
int  write(unsigned int fd, const void *buffer, unsigned int length);
long seek(unsigned int fd, long offset, unsigned char whence);
int  stat(const char *path, struct kfs_stat *out);
int  kos_opendir(const char *path);
int  kos_readdir(unsigned int fd, struct kfs_dirent *out);
int  kos_closedir(unsigned int fd);
int  mkdir(const char *path);
int  rmdir(const char *path);
int  unlink(const char *path);
int  rename(const char *oldpath, const char *newpath);
int  chdir(const char *path);
int  getcwd(char *buffer, unsigned int length);
int  sync(unsigned int fd);
```

Paths and buffers must live in the caller window (`0100h-1FFFh` for
paths, `0100h-DFFFh` for data); an over-long path or a bad window is
rejected locally with `-KOS_EINVAL` before the syscall.

`kos/process.h`:

```c
#define KOS_ARGV_MAX 16
struct kos_startup *startup(void);
int  splitargs(char *tail, char **argv, int max);
int  run(const char *path, const char *args);
```

`run` runs a child synchronously with the raw argument tail
(NULL/empty for none) and returns the child's exit status or `-errno`; the
tail reaches the child byte-exact through the startup block
(`docs/abi.md` section 11).

`kos/diag.h` (gate stubs: `diaggate.asm`) exposes the M7 recovery
boundary: `diag_mem_read`, `diag_mem_write`, `diag_io_in`,
`diag_io_out`, `diag_bank_info`, `diag_system_info`, with
their packed request blocks declared in `abi/kos/diag.h`.

`kos/ipc.h` (gate stub: `ipcgate.asm`, wrappers in `ipc.c`) is the M10
resident-driver client:

```c
#define KOS_IPC_CACHE_ENTRIES 8
struct kos_ipc_request {
	struct kos_ipc_frame frame;      /* 32 bytes, abi/kos/ipc.h    */
	void *buffer;                    /* captured descriptor buffer */
	unsigned int length;
	unsigned char direction;         /* KOS_IPC_DESC_IN / _OUT     */
};
int  kos_ipc_lookup(unsigned char class, unsigned char instance);
int  kos_ipc_lookup_version(unsigned char class, unsigned char instance,
			    unsigned char *version);
int  call(unsigned char handle, struct kos_ipc_request *req);
```

`user/sdk/src/kos_internal.h` (SDK-private, never installed) declares the
raw gate stub `kos_ipc_raw_call` and the `struct kos_ipc_call_req`
handling pair; a test that calls the stub directly adds `user/sdk/src` to
its `INCLUDES`.


`call` marshals the request into the exported block
`kos_ipc_call_block` and returns the IPC *status*: the driver's own
result, or a negated `KOS_E*` for an IPC-layer refusal (unknown handle,
forged descriptor, out-of-bounds copy, re-entrancy, nesting).  A
*structural* refusal (block outside the window, bad frame length or
reserved byte) is carried in `error` and mapped to `-errno`, so a driver
error is never confused with a malformed request.  `kos_ipc_lookup`
answers from a small (class, instance, handle) cache before it calls
`kos_ipc_lookup_version`, which always goes to the nucleus.

A call's optional buffer lives in the caller buffer window: the caller's
own bank `0100h-DFFFh`, or the common window `E000h-FFFFh`, which every
mapping shows at the same address.  The nucleus copies it on whichever of
the three paths (A, B, C; `docs/abi.md` section 12) the buffer's location
selects, so a client never has to think about where its data is - only a
*driver* has a placement rule (its own I/O buffers belong in bank 6's
`8000h-BFFFh`; `docs/abi.md` section 12).

Like every other wrapper, the IPC pair marshals through an argument block
inside the caller window: `kos_ipc_lookup_block` (4 bytes),
`kos_ipc_call_block` (44) and `kos_ipc_raw_req` (4) are exported so a
program's own acceptance check can prove their placement.  `runtime.asm`
owns them beside `kos_scratch` in one fixed 93-byte `_KOS_SDK_DATA` area;
ordinary program BSS may grow beyond `2000h` without moving a gate-visible
block.  `docs/abi.md` section 12 states the ABI rule.

When the build defines `KOS_NS_HANDLES` (the `namespace-selftest`
acceptance image, `docs/abi.md` section 12.1) the same header adds the
typed-namespace client, which replaces the lookup pair in package 42b:

```c
int bind(const char *path, unsigned int flags,
		 unsigned char *type);           /* bind, type out      */
int unbind(unsigned char handle);        /* release early       */
int list(const char *path, unsigned int *cookie,
		struct kos_ns_entry *out, unsigned int max_entries);
```

`call` is unchanged: the handle it names is now the *object
handle* `bind` returned rather than a looked-up endpoint, and endpoint
numbers never reach a program.  The two standard handles
(`KOS_HANDLE_STDOUT` 1, `KOS_HANDLE_STDIN` 2) exist before the first
`bind`, so the console client of that build writes through handle 1 with
no lookup at all (`console_proto.c` is guarded the same way), and a
child started with `run` receives its own pair and none of the
parent's bound handles.  The bind/unbind/ns_enum argument blocks live in
`nsgate.asm` (8 + 1 + 12 bytes), which the archive carries as its own
member and no other build links, so that variant's fixed low area is 89
bytes and its default code base moves to `0x0159` (`exe.mk`
`EXE_SDK_LOW_SIZE`, `EXE_DEFAULT_CODEBASE`).

`kos/xmem.h` (`xmem.c`) wraps the extended-memory family
(`xalloc`/`_free`/`_read`/`_write`/`_info`/`_map`) and the two
register-only aperture helpers `aperture_open(token)` /
`aperture_close(previous)` (`docs/abi.md` section 14).
`kos/seg.h` (`seg.c`) wraps the EXE v2 segment family:

```c
int seginfo(unsigned int segment, struct kos_seg_info *out);
int segmap(unsigned int segment);   /* FAR_CODE token, >= 1      */
typedef int (*kos_far_fn)(int, int);
int farcall(unsigned int segment, kos_far_fn entry, int a, int b);
```

`farcall` is the **far-call helper** (architecture 36.17): it maps
the program's own `FAR_CODE` segment, opens the aperture onto it, makes
the plain `CALL` to the linked far entry and restores the caller's latch
exactly.  The primary half takes the far entry's address from the
program's own link, so no relocation and no address arithmetic is
involved; the far half may call primary code directly (its address is
outside the aperture) and issue its own syscalls, whose buffers follow
the buffer-identity rule while the aperture is open.  An SDCC indirect
call to an address the compiler cannot see needs the trampolines in
`sdcc_calls.asm` (`___sdcc_call_iy`/`___sdcc_call_hl`), which is why the
archive carries that one assembly member.

## 7. Memory layout

```text
0100h-015Ch           fixed SDK low data (`_KOS_SDK_DATA`, 93 bytes: the
                      console, IPC, namespace-enumeration and exec blocks)
015Dh                 crt0 (kos_entry) - the ordinary entry offset is 5Dh
0100h + image_size    end of the linked image (s__DATA), start of BSS
                      crt0's tail pointer, its 127-byte tail copy and its
                      argv store, the shared scratch, then the program's
                      own statics
                      stack: 256 bytes below the stack top E000h (the
                      EXE header's stack_size; the kernel reserves it)
0100h-1FFFh           syscall argument window: the shared scratch and
                      every libkos marshalling block (the FS block, the
                      IPC lookup/call blocks) must live entirely inside it
8000h-BFFFh           the aperture slice: with the aperture open this
                      names a far bank - the program's FAR_CODE segment
                      (linked at 8000h) or a mappable object
```

The rule links `runtime.rel` explicitly in every program and fixes its
`_KOS_SDK_DATA` area before `_CODE`.  `kos_scratch` and all three IPC
blocks therefore stay in the gate window even for a large-BSS program such
as `fstest.exe`.  Omitting `-b _DATA` still lets the linker place ordinary
`_DATA` directly after `_CODE`, which keeps
`image_size = s__DATA - 0100h` true by construction; the packer verifies
it and `exemanifest` keeps its explicit scratch-window check.

A program linked with `NO_CRT0=1` owns its entry (`ENTRY=`) but still gets
the runtime's fixed low-data area; `SCRATCH=` may disable only the manifest
pin.  The IPC caller-buffer probe sets `LOWBASE=0x0300` and
`CODEBASE=0x0344`, which leaves `0100h-02FFh` as a plain
512-byte buffer: it is the only way a EXE can name the caller
window itself as a copy destination, and the only program in the
tree that links that way.

A program built with `FAR=` moves its far code out of this map entirely:
the `_FAR_CODE` area is linked at `8000h` and becomes a `FAR_CODE`
segment, so the primary image, its BSS and the whole stack stay inside
`0100h-7FFFh`/`C000h-DFFFh` and the aperture `8000h-BFFFh` is free for
the far bank.  `fardemo` (section 10) is the demo; the far code is
executed in place through the mode-10 aperture, so it costs no primary
bank space at all.

**One `_FAR_CODE` area per program link.**  The rule creates exactly one
far area, and two areas at the same base in one link emit overlapping IHX
records, which no loader can use.  Several far *segments* are several
`FAR_CODE` rows that name that one area (each row is placed in its own
bank; `tests/xmem/xmem_far.segments.json` declares the area twice to
prove two far segments in two banks).  A program that builds its own far
areas declares each one under its own name and based at `8000h`: the
manifest's `SEGMENTS=` rows name the areas, and the packer resolves
`s__<area>`/`l__<area>` and refuses an area based anywhere else.  The
name is what tells the areas apart in the manifest; all of them load at
`8000h` of their own far bank.

## 8. Which member pulls in what

`libkos` is an archive: a member is only linked when a symbol it defines is
still undefined.  The observed pulls (from the link maps) are:

| Program calls | Members linked | Notes |
| --- | --- | --- |
| anything | `kos_crt0`, `kos_runtime`, `args` | crt0 and the runtime object are unconditional; `args` supplies `splitargs` (crt0 calls it) |
| `console_write`, `console_read`, `console_poll`, `putchar` | `console_driver` | console-driver client (package 41) |
| `getinfo`, `exit` | `kos_gate` | exit/getinfo gate stubs, one object |
| `putchar`, `console_puts`, `getchar`, `console_readline` | `console_io` | built on the console client |
| `open`, `read`, ... | `fs` + `fsgate` | the C wrappers and the gate stub |
| `kos_ipc_lookup`, `call` | `ipc` + `ipcgate` | the resident-driver client and its gate stub |
| `bind`, `unbind`, `list` | `ipc` + `ipcgate` + `nsgate` | the `KOS_NS_HANDLES` variant: the three object/namespace blocks (`nsgate` is a member only that variant links; the runtime's stage-3 blocks stay in `runtime`) |
| `run` | `exec` + `fsgate` | shares the FS argument block and gate |
| `seginfo`, `segmap` | `seg` | the EXE v2 segment record and the `FAR_CODE` aperture token; shares the raw gate stub |
| `farcall` | `seg` + the program's far objects | maps, calls and restores; the far entry's address comes from the program's own link |
| an indirect call the compiler cannot bind at link time | `sdcc_calls` | `___sdcc_call_iy`/`___sdcc_call_hl` trampolines (SDCC emits these calls when a function pointer is not a constant) |
| `diag_*` | `kos_diaggate` | the diagnostic gate stubs |
| `printf` | `printf`, `strlen`, and the `*oa` formatters it needs (`utoa`, `ultoa`, `itoa`, `ltoa`) | `printf` is the largest member; a program that only needs a greeting can use `console_puts` and link none of it |
| `/` or `%` on any integer type | `sdcc_arith` / `sdcc_arith_signed` (+ their `*_entries` stubs) | only when the compiler emits a helper call |
| `strcmp`, `memset`, ... | that one member | one function per member |

`hello.exe` links 13 members (crt0, runtime, the program, `args`,
`console_driver`, `printf`, `strlen` and the four formatters plus the
arithmetic stubs) and no filesystem or diagnostic member at all;
`tests/test_sdk.py` asserts exactly that.

## 9. Boundary tests

`tests/test_source_layout.py` and `tests/test_sdk.py` enforce the rules
above:

- every declared program's first source includes `kos.h`, every quoted
  include resolves inside `{program dir, user/sdk/include, abi,
  INCLUDES=...}` and none resolves into `include/`, `kernel/`, `drivers/`,
  `platform/`, `fs/` or `drivers/` - `FAR=` sources exactly as the
  ordinary ones;
- `exe.mk` compiles with `-I abi -I $(KOS_SDK_INCLUDE)` and nothing else,
  compiles far sources with `--codeseg FAR_CODE` and bases `_FAR_CODE` at
  `8000h`;
- `exepack` refuses a `FAR_CODE` row that is not based at `8000h`, over
  16 KiB, overlapping another far area, or whose program's primary image,
  BSS or stack reaches into `8000h-BFFFh`;
- `libkos` has one member per source, no kernel object, and no member
  named after kernel code;
- `hello.exe`'s manifest equals what its link map implies, the container
  validates, the entry is crt0 at `0144h`, the scratch is inside the
  window, and no FS/diag member is linked;
`make check` builds `build/user/hello.exe` so these run in the standard
host check; the image test is added by `make helloworld-image`.

- the simulator runs the linked `hello.exe` bytes: `_splitargs` turns a
  quoted tail into the documented `argv` (with `argc` counted past the
  16-entry store), and `make helloworld-image` boots `hello.exe` as
  `/system/hello.exe` through the unmodified launcher, printing the
  greeting and then the exit report before the program is re-executed.

## 10. Bootable demo image

`user/helloworld/` builds `hello.exe`, and

```sh
make helloworld-image
```

builds a separate tree (`build-helloworld/`) from
`systems/helloworld.json`, whose `boot` is `/system/hello.exe` and whose
store adds `hello.exe` to the production file set.  Nothing else differs
from production: no kernel edit, no new syscall, no new module.  Booting that
image is the end-to-end proof that a user program inherits the boot path,
and it doubles as the transcript `tests/test_sdk.py` replays.

Because the launcher re-execs on every clean exit (section 4), the demo's
transcript on hardware **is** the repeating pair

```text
Hello, KonaOS!
monitor: exit status 0x0000 active 0 free-banks 6
```

The repetition is the evidence, not a fault: it is `hello.exe` being
loaded from the recovery store, run, exited and torn down by the
unmodified 28b launcher, with the counters constant every cycle.  A
single, non-repeating line would mean the program never exited.

The same image also carries **`fardemo.exe`** (`user/helloworld/fardemo.c`
and its far half `fardemo_far.c`, declared with
`FAR=fardemo_far.c,STACK=256,STAGE=1`): the smallest complete far-code
program.  Its primary half maps its own `FAR_CODE` segment with
`segmap`, calls the far entry through `farcall` and compares the
returned latch with the token; the far half writes its line from a
literal inside the aperture, calls back into primary code and returns
`(latch << 8) | value`.  Booting an image whose boot program is
`/system/fardemo.exe` (a system file whose driver set and store are the
helloworld image's and whose `boot` names `fardemo.exe`; `make
BUILD=build-fardemo SYSTEM=<that system>` builds it) prints

```text
fardemo: begin
fardemo: token 50
fardemo: hello from the far bank
fardemo: far result 502B
fardemo: PASS
```

The token is the aperture latch `P = 7 - bank` for the bank the loader
chose (bank 5 here, so page `2`); the far call returns that latch plus the
primary value the far half computed.  The far code runs in place through
the aperture, so none of it is in the primary image, and the packer proved
before the link that this program's image, BSS and stack stay outside
`8000h-BFFFh`.

## 11. M10 and later

`user/ZShell/` (M10) is built only from this SDK: one `exe_program` line
per program, no entry assembly, no kernel header.  It is the reference
implementation for a large SDK program (line editing, filesystem use,
`run` for its commands), so anything it cannot express is an SDK gap
to fix in `user/sdk/` rather than a reason to bypass it.  M10 then sets
`"boot": "/system/zshell.exe"` in the shell image's system file; the
shell needs no other kernel change.
