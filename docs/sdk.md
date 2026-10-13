# The KonaOS user SDK

`user/sdk/` is the only supported way to build a native KonaOS program --
the tree's own copy of it here, or the packaged standalone SDK (`make sdk`,
section 11), which builds a program with no OS source.
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
$(eval $(call exe_program,hello,user/sdk/examples,hello.c,STACK=256,STAGE=1))
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
  src/bcall.asm           ___sdcc_bcall_ehl, the banked-call trampoline
  src/console_proto.c   console-driver client (write/read/poll/putc)
  src/console_io.c        putchar/console_puts/getchar/console_readline
  src/klibc/              one archive member per function
  src/kos_internal.h      SDK-private glue (not for programs)
  exe.mk                  the in-tree wrapper: sets the in-tree roots,
                          builds libkos from src/, includes mk/kos.mk
  template/               Makefile + hello.c: the copyable minimal program
  examples/               hello, fardemo (__banked) and files (open/read/
                          list), each with the same standalone Makefile

mk/kos.mk                 the generic exe_program build rule (work package
                          49c); the SDK ships it and user/sdk/exe.mk wraps it

user/monitor/             monitor program + monitor-private helpers
user/helloworld/          the demo image's Makefile and README; its programs
                          are the SDK's examples/ above
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
include user/sdk/exe.mk          # in the OS tree; the packaged SDK uses
$(eval $(call exe_program,NAME,DIR,SOURCES[,OPTION=VALUE ...]))
```

The generic rule lives in `mk/kos.mk`, the file the standalone SDK ships
(work package 49c); `user/sdk/exe.mk` is the in-tree wrapper that sets the
in-tree roots and builds `libkos` from `src/`, so both ways run the same
rule:

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
| `CODEBASE=ADDR` | `0x015D` | base of executable code, immediately after the ordinary low-data area (93 bytes at `LOWBASE`) |
| `STAGE=1` | off | copy the packed program into `$(MODULES)` so a recovery manifest can name it |
| `INCLUDES=a:b` | empty | extra include roots (colon separated), for test-owned headers only |
| `SEGMENTS=FILE` | none | a JSON list of EXE v2 segment declarations, for hand-written layouts: `DATA` rows (`id`, `kind`, `input`, `bss_size`, and `flags: 1` for far data) and `FAR_CODE` rows (`id`, `kind`, `area`, `bss_size`); `FILE` is a build prerequisite and each payload `input` resolves against the program's object directory |
| `DATA1=SRC:SRC` … `DATA4=SRC:SRC` | none | far-data sources (relative to `DIR`) of the `DATAn` segment n (n = 1..4): a `.c` source is compiled with `--constseg DATA<n> --dataseg DATA<n>B`, and the link bases `_DATA<n>` **and** its BSS area `_DATA<n>B` at `8000h`, image first (section 13).  The options are numbered `1..m` without gaps; without `SEGMENTS=` the rule declares one mappable `DATA` row per option used, after the far-code rows |
| `BLOBS=a.bin:b.bin` | none | payload files (relative to `DIR`, at most 32 KiB each) the rule copies into the object directory and declares as plain `DATA` rows after the `DATAn` rows, in the order given; `blob(n)` returns the n-th one's handle (section 13) |
| `FAR1=SRC:SRC` … `FAR4=SRC:SRC` | none | far-code sources (relative to `DIR`) of far segment n (n = 1..4), placed in `_FAR<n>`, which the link bases at `8000h`; a `.c` source is compiled with bank n and `--codeseg FAR<n>`, so every `__banked` definition in it emits `b_<fn> = n`, and an `.asm` source opens `.area _FAR<n>` itself.  Without `SEGMENTS=` the rule declares one `FAR_CODE` segment per option used, id n, area `_FAR<n>` |

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
table and the payloads (`docs/exe-format.md`).  The far code segments
(`FAR1=`..`FAR4=`) are the container's first segments, ids 1..k in
option order, and the `SEGMENTS=` `DATA` rows follow them (ids k+1..N).
The loader numbers rows `1..N` in order, and a far segment's id is the
number its `__banked` definitions name (package 49b), so a program with
far code declares its `FAR_CODE` rows first.

A rule line with `DATA1=`..`DATA4=` (far data) or `BLOBS=` (payload
files) declares segment rows of its own: the far-code rows first, then
one **mappable** `DATA` row per `DATAn=` option in ascending order, then
one plain `DATA` row per `BLOBS=` file in the order given, so the ids a
`farmap(n)`/`blob(n)` ordinal counts are the generator's.  A `DATAn`
source holds `const` tables and zeroed objects only: SDCC puts the
`const` objects in `_DATA<n>` (the row's image, linked at `8000h`) and
the zeroed ones in `_DATA<n>B` (the row's BSS, the area the linker
concatenates right after the image, which the loader zeroes).  An
initialized **writable** object has no startup support in EXE v2 - SDCC
would put its value in `_INITIALIZED` - so `exepack` refuses it and names
the symbol; initialize such data at run time.  `DATAn=` and `FARn=`
sources may share a program, and the pair is limited to the 16 KiB
aperture exactly as a far-code area is.

A rule line with `FAR1=`..`FAR4=` also switches on the **36.17 link
rule**: the packer proves the primary image and BSS end below `8000h` and
the stack base sits at or above `C000h`, so a program that uses far code
keeps `8000h-BFFFh` for the aperture alone.  `fardemo` (section 10) is
the smallest program that links that way, and its far half is called as
plain C: the far source's `__banked` definition makes SDCC emit
`ld e,#b_<fn>` / `ld hl,#_<fn>` / `call ___sdcc_bcall_ehl`, and the SDK's
trampoline (`src/bcall.asm`) maps segment `E`, opens the aperture onto
it, transfers to `HL` and restores the caller's latch.  `farcall` stays
as the primitive that takes an explicit segment and entry address.

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
#define KOS_RT_EFARMAP 0x30         /* a banked call named no far segment */
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

`__banked` is the same mechanism as plain C (package 49b).  A program
built with `FAR1=`..`FAR4=` calls a far function whose definition is
`__banked` like any other function:

```c
extern int far_main(int a, int b) __banked;  /* built with FAR1= */
result = far_main(a, b);
```

`bcall.asm`'s `___sdcc_bcall_ehl` is the trampoline SDCC's calling
convention names.  Arguments travel on the stack exactly as SDCC pushed
them, the result comes back in `A` (8 bits), `DE` (16) or `DEHL` (32),
and everything else the caller needs is spilled before the call.  The
trampoline keeps one aperture token per segment id, so only the first
call of a segment spends a `SYS_SEG_MAP`; a far function may make banked
calls of its own, into its own or another segment, and each one restores
its caller's mapping on return.  A banked call that names no far segment
- an id the container does not have, or a `__banked` definition in the
primary bank (`b_` = 0) - reports `KOS_RT_EFARMAP` (an SDK-level code,
outside the syscall-visible `KOS_E*` range) through `exe_runtime_error`
and does not run the function.

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

A program built with `FAR1=` (up to `FAR4=`) moves its far code out of
this map entirely: the `_FAR1` area is linked at `8000h` and becomes the
`FAR_CODE` segment 1, so the primary image, its BSS and the whole stack
stay inside
`0100h-7FFFh`/`C000h-DFFFh` and the aperture `8000h-BFFFh` is free for
the far bank.  `fardemo` (section 10) is the demo; the far code is
executed in place through the mode-10 aperture, so it costs no primary
bank space at all.

**One far area per segment, all at `8000h`.**  The rule creates one area
per `FAR<n>=` option used, `_FAR<n>`, each based at `8000h`.  The linker
writes one record stream per area, in link order (ascending id), so the
n-th byte of an address is the n-th area covering it and the packer takes
each area's payload from its own stream (package 49b).  A program that
builds its own far areas declares each one under its own name and based at
`8000h`: the manifest's `SEGMENTS=` rows name the areas, the packer
resolves `s__<area>`/`l__<area>` and refuses an area based anywhere else,
and a `FAR_CODE` row's id is the number in its area name (`_FAR<n>` is
segment n, and the far rows are the container's first).  All of the areas
load at `8000h` of their own far bank.

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
  `platform/`, `fs/` or `drivers/` - far sources (`FAR1=`..`FAR4=`)
  exactly as the ordinary ones;
- `exe.mk` compiles with `-I abi -I $(KOS_SDK_INCLUDE)` and nothing else,
  compiles each far source with bank n and `--codeseg FAR<n>` and bases
  `_FAR<n>` at `8000h`;
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

`user/helloworld/Makefile` declares the SDK's own examples
(`user/sdk/examples/hello.c`, staged as `/system/hello.exe`), and

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

The same image also carries **`fardemo.exe`** (`user/sdk/examples/fardemo.c`
and its far half `fardemo_far.c`, declared with
`FAR1=fardemo_far.c,STACK=256,STAGE=1`): the smallest complete far-code
program, and since package 49b the smallest **banked call**.  Its primary
half maps its own `FAR_CODE` segment with `segmap` and calls the
`__banked` far entry as plain C, whose trampoline maps the segment and
restores the latch; it compares the returned latch with `segmap`'s token.
The far half writes its line from a literal inside the aperture, calls
back into primary code and returns `(latch << 8) | value`.  Booting an image whose boot program is
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

The examples also carry **`fardata.exe`** (`user/sdk/examples/fardata.c`
with `levels.c` and `fardata_far.c`, declared with
`DATA1=levels.c,FAR1=fardata_far.c,STACK=256`): the smallest complete
far-data program (section 12).  Its primary half maps the `DATA1` segment
with `farmap`, reads the 4 KiB table and writes the zeroed 2 KiB array
through the window, restores the caller's latch, and only then makes the
`__banked` call with two values copied out of the table - the one-window
rule, both halves in one program.  `make` in `user/sdk/examples` builds
it beside the other examples, so `make sdk` ships it and the SDK's own
test run compiles it out of tree.

## 11. Using the standalone SDK

```sh
make sdk        # in the OS tree: build/kos-sdk-<version>/ + .tar.gz
```

packages the SDK for a developer who has no OS source.  Repacking
unchanged sources reproduces the package byte for byte: the shipped
`lib/libkos.lib` is archived with zeroed member dates and ids (`sdar
-rcD`) and the tarball pins its member order, mtimes and ownership.  The
tree holds everything a program needs and nothing from the tree it was
built in:

```text
kos-sdk-<version>/
  VERSION          "<version> abi <n>", n = the loader's EXE_ABI_VERSION
  include/         kos.h, kos/*.h, klibc/*.h, include/abi/kos/*.h
  lib/             libkos.lib (the runtime object folded in), crt0.rel
  tools/           exepack.py, exemanifest.py, ihx.py (standalone)
  mk/kos.mk        the standalone exe_program rules
  template/        Makefile + hello.c: the smallest program
  examples/        hello, fardemo (__banked), files (open/read/list)
  docs/            sdk.md, exe-format.md
```

Every header keeps the path its includes name: the SDK's own sit at
`include/kos/…` and `include/klibc/…`, and the ABI headers they include
are copied to `include/abi/kos/…`, the first include root (`-I
include/abi -I include`), exactly the order the in-tree build uses.

The four steps from unpacking to running the program:

1. **unpack** the tarball, then `cd` into `template/` (or any directory
   holding a `Makefile` that includes the SDK's `mk/kos.mk`; `examples/`
   has one);
2. **`make`** — `exe_program` compiles the sources, links `crt0.rel` and
   `libkos.lib` from `lib/`, and packs `build/user/<name>.exe`.  A
   program Makefile names the SDK with `KOS_SDK_ROOT` (default `..`) and
   needs nothing else;
3. **copy the `.exe` to the card** (the FAT filesystem the iocfat driver
   serves, `/fs`);
4. **run it** from the monitor with `X /fs/<name>.exe`, or from the
   shell as `<name>`.

A new program is a copy of `template/` (Makefile + `hello.c`) with its
name in the `exe_program` line.  The examples show the rest of the
surface: `examples/hello.c` prints its arguments, `examples/fardemo.c`
plus `examples/fardemo_far.c` call a `__banked` function across the far
aperture, and `examples/files.c` opens, reads and lists the store.

`tests/test_sdk_package.py` is the package's acceptance: it builds the
tarball, unpacks it outside the repository, checks the file list, the
header closure, the ABI number and the archive's pinned metadata, builds
template/ and examples/ with the tree's own include and library paths
absent from the environment, runs the result in the simulator through
the production image's monitor, and proves a copy with a changed ABI
byte is refused.

## 12. Far data

Far data is any table or buffer a program needs that does not fit in the
56 KiB it owns - level maps, fonts, wave tables, whole files.  There are
four ways in, and they differ in *when* the bytes arrive and *how* they
are reached (work package 49d):

| Kind | Declared | Reached | Costs |
| --- | --- | --- | --- |
| `DATAn=` sources | build time, in C | `farmap(n)` opens the window; the sources' C symbols are plain pointers while it is open | one auxiliary bank per `DATAn`; the bank is the program's for its whole life |
| `BLOBS=` files | build time, raw files | `blob(n)` returns the handle; `xread`/`xwrite` copy through it | no bank of its own; a copy per access |
| `xalloc` | run time | `xmap` + `aperture_open` for a mappable object, or `xread`/`xwrite` by handle | a bank for a mappable object, slots alone otherwise |
| `xload` | run time, a file the program names | the returned handle, as `xalloc`'s | one copy of the whole file into an object |

`farmap(n, &saved)`/`farunmap(saved)` are the pointer route:

```c
unsigned char saved;
const unsigned char *table = farmap(1, &saved);   /* the DATA1 segment */

if (table == (const unsigned char *)0) {
	/* the refusal is in `saved` as a negated error code */
	return 1;
}
level = table[level_index];      /* a plain array access */
farunmap(saved);                 /* restore the caller's mapping */
```

A `const` table written this way costs the primary image nothing: it is
linked at `8000h` and lives in its own bank, and the loader puts it
exactly where the link placed it.  The zeroed half of the same source is
reached through the same window (the loader zeroes the segment's BSS), so
scratch buffers that would not fit in the primary bank work the same way.
`xread`/`xwrite` reach the same segment by handle when copying suits
better than mapping.

**One window.**  The machine has a single far window, `8000h-BFFFh`, and
far code shares it with mapped far data: a `__banked` function and a
mapped `DATAn` segment cannot be visible at the same time.  Primary code
may map data and use pointers; a `__banked` function reaches far data
only by copy - `xread`, or a pointer the primary hands it **after**
copying the bytes out.  `farunmap` before a banked call, `farmap` again
after: each mapper restores exactly the latch it saved, so the window can
be handed back and forth.

**Bank budget.**  Every far-code segment and every mappable data segment
(or mappable `xalloc` object) takes one auxiliary bank for its own
aperture slice, and there are six application banks - the program's own
primary bank is never available to them.  Five far or mappable segments
is therefore the ceiling for one program, and a container that asks for
more is refused when it is run: `run` returns `-KOS_ENOSPC`, nothing is
allocated, and the program keeps the banks it had.

**Costs.**  A banked call is a trampoline plus an `OUT` and a plain
`CALL` - about 31 µs measured on the reference machine - and only the
first call of a segment spends a `SYS_SEG_MAP`.  Access through an open
window costs nothing extra: `table[i]` is one load, exactly as if the
table were in the program's own bank.  `xread` costs 21 T-states per byte
in the window-speed slots and 287 T-states per byte otherwise, so a
program that walks a large table once should map it, and one that reads a
few bytes from many places should copy.

`user/sdk/examples/fardata.c` with `levels.c` (a 4 KiB `const` table and
a zeroed 2 KiB array) is the smallest complete far-data program; section
10 lists it beside the far-code demo.  A file that only arrives at run
time is `xload`'d when it fits (32 KiB, or 16 KiB when the object must be
mappable) and streamed with `open`/`read`/`xwrite` when it does not.

## 13. M10 and later

`user/ZShell/` (M10) is built only from this SDK: one `exe_program` line
per program, no entry assembly, no kernel header.  It is the reference
implementation for a large SDK program (line editing, filesystem use,
`run` for its commands), so anything it cannot express is an SDK gap
to fix in `user/sdk/` rather than a reason to bypass it.  M10 then sets
`"boot": "/system/zshell.exe"` in the shell image's system file; the
shell needs no other kernel change.
