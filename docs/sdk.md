# The ZephyrOS user SDK

`user/sdk/` is the only supported way to build a native ZephyrOS program.
A program is a C file with `int main(int argc, char **argv)`, one line in a
Makefile, and nothing else: no entry assembly, no hand-written manifest, no
kernel header, no hosted C library.

```c
#include "zos.h"

int main(int argc, char **argv)
{
	zos_printf("Hello, ZephyrOS!\r\n");
	return 0;
}
```

```make
$(eval $(call zex_program,hello,user/helloworld,hello.c,STACK=256))
```

Package 28c introduced this layout; the public ABI it compiles against
lives once, in `abi/zos/` (see `docs/abi.md` for the ABI itself and
`docs/zex-format.md` for the container format).  `user/ZShell/` (M10) must
be built from the SDK alone and is the SDK's reference implementation.

## 1. Layout

```text
abi/zos/                  the public native ABI (one copy, all builds)
  types.h errno.h syscall.h fs.h process.h diag.h

user/sdk/
  include/zos.h           the umbrella header a program includes
  include/zos/            console.h fs.h process.h diag.h
  include/zlibc/          string.h fmt.h stdarg.h
  src/crt0.asm            C startup: startup block, argv, main, SYS_EXIT
  src/runtime.asm         shared argument scratch + error mapper
  src/gate.asm            exit/getinfo gate stubs
  src/fsgate.asm          filesystem gate stub
  src/diaggate.asm        diagnostic gate stubs
  src/args.c              zos_args(), zos_split()
  src/fs.c                zos_open/zos_read/... wrappers
  src/exec.c              zos_exec()
  src/console_service.c   console-service client (write/read/poll/putc)
  src/console_io.c        zos_putc/zos_puts/zos_getc/zos_read_line
  src/zlibc/              one archive member per function
  src/zos_internal.h      SDK-private glue (not for programs)
  zex.mk                  the generic zex_program build rule
  template/hello.c        the copyable minimal program

user/monitor/             monitor program + monitor-private helpers
user/helloworld/          demos (hello.c) and their Makefile fragment
```

`user/monitor/` keeps only what is specific to the monitor: its command
engine, `hexfmt`, `hexdump` and `status` output.  Anything reusable lives
in the SDK.

## 2. Include rules

A program compiles with exactly:

```text
sdcc -mz80 -c --nostdinc --Werror -I abi -I user/sdk/include -I <program dir>
```

- **`#include "zos.h"`** is the only header a program needs; it pulls in
  `zos/console.h`, `zos/fs.h`, `zos/process.h`, `zos/diag.h`,
  `zlibc/string.h` and `zlibc/fmt.h`.
- The SDK's `zos/*.h` headers include the matching **`abi/zos/*.h`**
  header, so the ABI declarations exist exactly once.  SDCC resolves a
  quoted include in the including file's own directory first, which is why
  `user/sdk/include/zos/fs.h` gets `abi/zos/fs.h` (and not itself).
- A program may include its own local headers (`monitor.h`,
  `hexdump.h`, ...).  A test program may add a narrow test-owned include
  root with `INCLUDES=`.
- A program must **never** include `include/zephyros/`, `kernel/`,
  `drivers/`, `platform/`, `fs/` or `services/`.  `-I` is not even given
  for them, and `tests/test_sdk.py` resolves every quoted include of every
  declared program to prove it.
- There is no hosted C library: `--nostdinc` plus `zlibc` (section 5)
  replace its string, memory and formatting parts.  No `malloc`, no stdio
  streams, no floating point, no POSIX emulation.

## 3. Build rule: `zex_program`

```make
include user/sdk/zex.mk
$(eval $(call zex_program,NAME,DIR,SOURCES[,OPTION=VALUE ...]))
```

`SOURCES` are relative to `DIR`; `.c` files go through SDCC, `.asm` files
through `sdasz80`.  The rule compiles them, links crt0 and the shared
runtime object, then the program objects, then `libzos.lib`, derives the
manifest from the link map, packs and validates
`build/user/NAME.zex`, and keeps every intermediate under
`build/user/NAME/`, so programs can be built in parallel.

| Option | Default | Meaning |
| --- | --- | --- |
| `STACK=N` | `256` | child stack reservation in the manifest |
| `SCRATCH=SYM:SIZE` | `zos_scratch:16` | scratch symbol the packer must find in `0100h-1FFFh`; `SCRATCH=` declares none |
| `ENTRY=SYM` | `zos_entry` | entry symbol (crt0's entry) |
| `NO_CRT0=1` | off | link no crt0; `ENTRY` names the program's own entry (the fixed SDK low-data/runtime object remains) |
| `LOWBASE=ADDR` | `0x0100` | base of the SDK's fixed gate-visible data area (93 bytes: the object/namespace enumeration and exec blocks are part of it) |
| `CODEBASE=ADDR` | `0x0144` | base of executable code, immediately after the ordinary low-data area (`0x0159` in the `ZOS_NS_HANDLES` variant) |
| `STAGE=1` | off | copy the packed program into `$(MODULES)` so a recovery manifest can name it |
| `INCLUDES=a:b` | empty | extra include roots (colon separated), for test-owned headers only |
| `SEGMENTS=FILE` | none | a JSON list of ZEX v2 `DATA` segment declarations (`id`, `kind`, `input`, `bss_size`); `FILE` is a build prerequisite and each payload `input` resolves against the program's object directory |

The rule file is a prerequisite of the archive and of every link script,
so editing it (a new source, a changed flag) re-archives `libzos` from
scratch and relinks each program; a stale archive can never keep a removed
member alive.

There is **no hand-written manifest**: `tools/zexmanifest.py` computes
`image_size = s__DATA - 0100h`, `bss_size = l__DATA` and
`entry_offset = ENTRY - 0100h` from the link map, and `zexpack` validates
the packed container against the map's symbols (including the scratch
window rule).  `SEGMENTS=FILE` merges auxiliary segment declarations;
the packer derives each segment's `image_size` from its payload file and
lays out the 36-byte v2 header, the table and the payloads
(`docs/zex-format.md`).

## 4. crt0, argv and exit

The kernel's launch gate enters the program at `zos_entry` (crt0, linked
first so it lands at `0100h`) with `HL = 0080h`, the versioned startup
block (`docs/zex-format.md` section 5.1):

1. crt0 stores `HL` in `zos_startup_block`, so `zos_args()` can hand the
   raw block to a program later (the kernel's page-zero copy is not
   writable through it);
2. it validates `version == 1` and `tail_length <= 125`; anything else is
   treated as an empty command line rather than reading past the block;
3. it copies the tail into the SDK's own 127-byte buffer and calls
   `zos_split(tail, argv, ZOS_ARGV_MAX)` over that copy, which splits **in
   place** (the quotes, spaces and NULs are rewritten in program BSS);
   the startup block is an entry-time delivery, not durable storage (it
   lies below the `0100h` syscall buffer window), so `argv` must point
   into program data for the words to survive the syscalls `main()`
   makes;
4. it calls `int main(int argc, char **argv)` with `argc` clamped to
   `ZOS_ARGV_MAX` (16) even when more words were counted;
5. on return it calls `zex_exit` (`SYS_EXIT`) with the low 16 bits of the
   return value, and halts if that ever returns.

`argv` holds the command-tail words only: the kernel transports no program
name, so a bare `/SYSTEM/hello.zex` launch calls `main(0, argv)`.  The
splitter's quoting rules are documented in `docs/zex-format.md` section
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

## 5. `zlibc`

One archive member per function, so a program links only what it calls.
Nothing here allocates.

`zlibc/string.h`:

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

`zlibc/fmt.h`:

```c
void zos_utoa(char *dst, unsigned int value, unsigned char base,
              unsigned char width, char pad);
void zos_ultoa(char *dst, unsigned long value, unsigned char base,
               unsigned char width, char pad);
void zos_itoa(char *dst, int value, unsigned char base,
              unsigned char width, char pad);
void zos_ltoa(char *dst, long value, unsigned char base,
              unsigned char width, char pad);
int  zos_printf(const char *fmt, ...);
int  zos_snprintf(char *dst, unsigned int size, const char *fmt, ...);
```

`zos_printf` supports `%s %c %d %u %x %ld %lu %lx %%` with an optional
width and a `0` zero-pad flag (`%05d`, `%4s`); `zos_snprintf` is the same
formatter writing into a bounded buffer that is always NUL-terminated.
`zos_printf` writes through the console service (`zex_write`).

Integer division and modulo are a language feature, not a library
function: the compiler emits calls to `__divuint`, `__moduint`,
`__divulong`, `__modulong`, `__divsint` and friends.  `zlibc` provides
them (`sdcc_arith.c`, `sdcc_arith_signed.c` plus the assembly entry
stubs), implemented with shifts and subtraction so `libzos` itself needs
no division.

## 6. libzos API

The ABI's raw syscalls (`SYS_*`, `RST 30h`) are wrapped once, here.  Every
wrapper takes arguments in the documented native convention, keeps its
argument block in the shared low-window scratch, and returns the syscall
result or the negated `errno` (`-ZOS_EINVAL`, `-ZOS_EFAULT`, ...).

`zos/console.h` (console client: `console_service.c`; exit/getinfo gate stubs:
`gate.asm`):

```c
void zex_exit(unsigned int status);          /* SYS_EXIT, does not return */
int  zex_write(const char *buf, unsigned int len);  /* console service WRITE */
int  zex_read(char *buf, unsigned int len);         /* READ, non-blocking    */
int  zex_poll(void);                                /* POLL                  */
int  zex_putc(int ch);                              /* PUTC, no descriptor   */
int  zex_getinfo(struct zos_info_record *record);   /* SYS_GETINFO */
int  zos_putc(char ch);
int  zos_console_write(const char *buf, unsigned int len);
int  zos_puts(const char *text);
int  zos_getc(void);                          /* blocking */
int  zos_read_line(char *buffer, unsigned int capacity);
```

Package 41 removed the `SYS_CONSOLE_*` syscalls: the four `zex_*` console
entries are clients of the resident console service
(`abi/zos/protocol.h`, class `ZOS_CLASS_CONSOLE`), resolved once through
`SYS_IPC_LOOKUP` and cached; a program whose store lists no console
service sees `-ZOS_ENOENT`.  `zos_putc` uses the descriptor-less PUTC
operation, the rest one descriptor-bearing call each.

`zos_read_line` is bounded line editing with backspace and CR handling
(`ZOS_LINE_MAX` is 80); it is the monitor's input loop and any program's
cheap command reader.

`zos/fs.h` (gate stub: `fsgate.asm`, wrappers in `fs.c`):

```c
struct zos_fs_call_req { unsigned char number; void *block; };
int  zos_fs_call(struct zos_fs_call_req *req);   /* the single gate stub */

int  zos_open(const char *path, unsigned char flags);
int  zos_close(unsigned int fd);
int  zos_read(unsigned int fd, void *buffer, unsigned int length);
int  zos_write(unsigned int fd, const void *buffer, unsigned int length);
long zos_seek(unsigned int fd, long offset, unsigned char whence);
int  zos_stat(const char *path, struct zfs_stat *out);
int  zos_opendir(const char *path);
int  zos_readdir(unsigned int fd, struct zfs_dirent *out);
int  zos_closedir(unsigned int fd);
int  zos_mkdir(const char *path);
int  zos_rmdir(const char *path);
int  zos_unlink(const char *path);
int  zos_rename(const char *oldpath, const char *newpath);
int  zos_chdir(const char *path);
int  zos_getcwd(char *buffer, unsigned int length);
int  zos_sync(unsigned int fd);
```

Paths and buffers must live in the caller window (`0100h-1FFFh` for
paths, `0100h-DFFFh` for data); an over-long path or a bad window is
rejected locally with `-ZOS_EINVAL` before the syscall.

`zos/process.h`:

```c
#define ZOS_ARGV_MAX 16
struct zos_startup *zos_args(void);
int  zos_split(char *tail, char **argv, int max);
int  zos_exec(const char *path, const char *args);
```

`zos_exec` runs a child synchronously with the raw argument tail
(NULL/empty for none) and returns the child's exit status or `-errno`; the
tail reaches the child byte-exact through the startup block
(`docs/abi.md` section 11).

`zos/diag.h` (gate stubs: `diaggate.asm`) exposes the M7 recovery
boundary: `zex_diag_mem_read`, `zex_diag_mem_write`, `zex_diag_io_in`,
`zex_diag_io_out`, `zex_diag_bank_info`, `zex_diag_system_info`, with
their packed request blocks declared in `abi/zos/diag.h`.

`zos/ipc.h` (gate stub: `ipcgate.asm`, wrappers in `ipc.c`) is the M10
resident-service client:

```c
#define ZOS_IPC_CACHE_ENTRIES 8
struct zos_ipc_request {
	struct zos_ipc_frame frame;      /* 32 bytes, abi/zos/ipc.h    */
	void *buffer;                    /* captured descriptor buffer */
	unsigned int length;
	unsigned char direction;         /* ZOS_IPC_DESC_IN / _OUT     */
};
int  zos_ipc_lookup(unsigned char class, unsigned char instance);
int  zos_ipc_lookup_version(unsigned char class, unsigned char instance,
			    unsigned char *version);
int  zos_ipc_call(unsigned char handle, struct zos_ipc_request *req);
struct zos_ipc_call_req {            /* the raw pair, tests only     */
	unsigned char number; void *block; unsigned char error;
};
int  zos_ipc_raw_call(struct zos_ipc_call_req *req);
```

`zos_ipc_call` marshals the request into the exported block
`zos_ipc_call_block` and returns the IPC *status*: the service's own
result, or a negated `ZOS_E*` for an IPC-layer refusal (unknown handle,
forged descriptor, out-of-bounds copy, re-entrancy, nesting).  A
*structural* refusal (block outside the window, bad frame length or
reserved byte) is carried in `error` and mapped to `-errno`, so a service
error is never confused with a malformed request.  `zos_ipc_lookup`
answers from a small (class, instance, handle) cache before it calls
`zos_ipc_lookup_version`, which always goes to the nucleus.

A call's optional buffer lives in the caller buffer window: the caller's
own bank `0100h-DFFFh`, or the common window `E000h-FFFFh`, which every
mapping shows at the same address.  The nucleus copies it on whichever of
the three paths (A, B, C; `docs/abi.md` section 12) the buffer's location
selects, so a client never has to think about where its data is - only a
*service* has a placement rule (its own I/O buffers belong in bank 6's
`8000h-BFFFh`; `docs/abi.md` section 12).

Like every other wrapper, the IPC pair marshals through an argument block
inside the caller window: `zos_ipc_lookup_block` (4 bytes),
`zos_ipc_call_block` (44) and `zos_ipc_raw_req` (4) are exported so a
program's own acceptance check can prove their placement.  `runtime.asm`
owns them beside `zos_scratch` in one fixed 93-byte `_ZOS_SDK_DATA` area;
ordinary program BSS may grow beyond `2000h` without moving a gate-visible
block.  `docs/abi.md` section 12 states the ABI rule.

When the build defines `ZOS_NS_HANDLES` (the `namespace-selftest`
acceptance image, `docs/abi.md` section 12.1) the same header adds the
typed-namespace client, which replaces the lookup pair in package 42b:

```c
int zos_ipc_bind(const char *path, unsigned int flags,
		 unsigned char *type);           /* bind, type out      */
int zos_ipc_unbind(unsigned char handle);        /* release early       */
int zos_ns_enum(const char *path, unsigned int *cookie,
		struct zos_ns_entry *out, unsigned int max_entries);
```

`zos_ipc_call` is unchanged: the handle it names is now the *object
handle* `bind` returned rather than a looked-up endpoint, and endpoint
numbers never reach a program.  The two standard handles
(`ZOS_HANDLE_STDOUT` 1, `ZOS_HANDLE_STDIN` 2) exist before the first
`bind`, so the console client of that build writes through handle 1 with
no lookup at all (`console_service.c` is guarded the same way), and a
child started with `zos_exec` receives its own pair and none of the
parent's bound handles.  The bind/unbind/ns_enum argument blocks live in
`nsgate.asm` (8 + 1 + 12 bytes), which the archive carries as its own
member and no other build links, so that variant's fixed low area is 89
bytes and its default code base moves to `0x0159` (`zex.mk`
`ZEX_SDK_LOW_SIZE`, `ZEX_DEFAULT_CODEBASE`).

## 7. Memory layout

```text
0100h-015Ch           fixed SDK low data (`_ZOS_SDK_DATA`, 93 bytes: the
                      console, IPC, namespace-enumeration and exec blocks)
015Dh                 crt0 (zos_entry) - the ordinary entry offset is 5Dh
0100h + image_size    end of the linked image (s__DATA), start of BSS
                      crt0's tail pointer, its 127-byte tail copy and its
                      argv store, the shared scratch, then the program's
                      own statics
                      stack: 256 bytes below the stack top E000h (the
                      ZEX header's stack_size; the kernel reserves it)
0100h-1FFFh           syscall argument window: the shared scratch and
                      every libzos marshalling block (the FS block, the
                      IPC lookup/call blocks) must live entirely inside it
```

The rule links `runtime.rel` explicitly in every program and fixes its
`_ZOS_SDK_DATA` area before `_CODE`.  `zos_scratch` and all three IPC
blocks therefore stay in the gate window even for a large-BSS program such
as `fstest.zex`.  Omitting `-b _DATA` still lets the linker place ordinary
`_DATA` directly after `_CODE`, which keeps
`image_size = s__DATA - 0100h` true by construction; the packer verifies
it and `zexmanifest` keeps its explicit scratch-window check.

A program linked with `NO_CRT0=1` owns its entry (`ENTRY=`) but still gets
the runtime's fixed low-data area; `SCRATCH=` may disable only the manifest
pin.  The IPC caller-buffer probe sets `LOWBASE=0x0300` and
`CODEBASE=0x0344`, which leaves `0100h-02FFh` as a plain
512-byte buffer: it is the only way a ZEX can name the caller
window itself as a copy destination, and the only program in the
tree that links that way.

## 8. Which member pulls in what

`libzos` is an archive: a member is only linked when a symbol it defines is
still undefined.  The observed pulls (from the link maps) are:

| Program calls | Members linked | Notes |
| --- | --- | --- |
| anything | `zos_crt0`, `zos_runtime`, `args` | crt0 and the runtime object are unconditional; `args` supplies `zos_split` (crt0 calls it) |
| `zex_write`, `zex_read`, `zex_poll`, `zex_putc` | `console_service` | console-service client (package 41) |
| `zex_getinfo`, `zex_exit` | `zos_gate` | exit/getinfo gate stubs, one object |
| `zos_putc`, `zos_puts`, `zos_getc`, `zos_read_line` | `console_io` | built on the console client |
| `zos_open`, `zos_read`, ... | `fs` + `fsgate` | the C wrappers and the gate stub |
| `zos_ipc_lookup`, `zos_ipc_call` | `ipc` + `ipcgate` | the resident-service client and its gate stub |
| `zos_ipc_bind`, `zos_ipc_unbind`, `zos_ns_enum` | `ipc` + `ipcgate` + `nsgate` | the `ZOS_NS_HANDLES` variant: the three object/namespace blocks (`nsgate` is a member only that variant links; the runtime's stage-3 blocks stay in `runtime`) |
| `zos_exec` | `exec` + `fsgate` | shares the FS argument block and gate |
| `zos_seg_info` | `seg` | the ZEX v2 segment record; shares the raw gate stub |
| `zex_diag_*` | `zos_diaggate` | the diagnostic gate stubs |
| `zos_printf` | `printf`, `strlen`, and the `*oa` formatters it needs (`utoa`, `ultoa`, `itoa`, `ltoa`) | `printf` is the largest member; a program that only needs a greeting can use `zos_puts` and link none of it |
| `/` or `%` on any integer type | `sdcc_arith` / `sdcc_arith_signed` (+ their `*_entries` stubs) | only when the compiler emits a helper call |
| `strcmp`, `memset`, ... | that one member | one function per member |

`hello.zex` links 13 members (crt0, runtime, the program, `args`,
`console_service`, `printf`, `strlen` and the four formatters plus the
arithmetic stubs) and no filesystem or diagnostic member at all;
`tests/test_sdk.py` asserts exactly that.

## 9. Boundary tests

`tests/test_source_layout.py` and `tests/test_sdk.py` enforce the rules
above:

- every declared program's first source includes `zos.h`, every quoted
  include resolves inside `{program dir, user/sdk/include, abi,
  INCLUDES=...}` and none resolves into `include/`, `kernel/`, `drivers/`,
  `platform/`, `fs/` or `services/`;
- `zex.mk` compiles with `-I abi -I $(ZOS_SDK_INCLUDE)` and nothing else;
- `libzos` has one member per source, no kernel object, and no member
  named after kernel code;
- `hello.zex`'s manifest equals what its link map implies, the container
  validates, the entry is crt0 at `0144h`, the scratch is inside the
  window, and no FS/diag member is linked;
`make check` builds `build/user/hello.zex` so these run in the standard
host check; the image test is added by `make helloworld-image`.

- the simulator runs the linked `hello.zex` bytes: `_zos_split` turns a
  quoted tail into the documented `argv` (with `argc` counted past the
  16-entry store), and `make helloworld-image` boots `hello.zex` as
  `/SYSTEM/hello.zex` through the unmodified launcher, printing the
  greeting and then the exit report before the program is re-executed.

## 10. Bootable demo image

`user/helloworld/` builds `hello.zex`, and

```sh
make helloworld-image
```

builds a separate tree (`build-helloworld/`) with
`ZOS_BOOT_PROGRAM=/SYSTEM/hello.zex` and a recovery manifest that adds
`hello.zex` to the production file set.  Nothing else differs from
production: no kernel edit, no new syscall, no new module.  Booting that
image is the end-to-end proof that a user program inherits the boot path,
and it doubles as the transcript `tests/test_sdk.py` replays.

Because the launcher re-execs on every clean exit (section 4), the demo's
transcript on hardware **is** the repeating pair

```text
Hello, ZephyrOS!
monitor: exit status 0x0000 active 0 free-banks 6
```

The repetition is the evidence, not a fault: it is `hello.zex` being
loaded from the recovery store, run, exited and torn down by the
unmodified 28b launcher, with the counters constant every cycle.  A
single, non-repeating line would mean the program never exited.

## 11. M10 and later

`user/ZShell/` (M10) is built only from this SDK: one `zex_program` line
per program, no entry assembly, no kernel header.  It is the reference
implementation for a large SDK program (line editing, filesystem use,
`zos_exec` for its commands), so anything it cannot express is an SDK gap
to fix in `user/sdk/` rather than a reason to bypass it.  M10 then sets
`ZOS_BOOT_PROGRAM=/SYSTEM/zshell.zex` in `config/system.mk`; the shell
needs no other kernel change.
