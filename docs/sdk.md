# The KonaOS SDK

The only supported way to build a native KonaOS program: the tree's own
copy in `user/sdk/`, or the packaged standalone SDK (`make sdk`, section
11), which needs no OS source. A program is C with
`int main(int argc, char **argv)` and one line in a Makefile: no entry
assembly, no hand-written manifest, no kernel header, no hosted C library.

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

The ABI is in `docs/abi.md`, the container format in
`docs/exe-format.md`.

## 1. Layout

```text
abi/kos/                  the public ABI (one copy for every build)

user/sdk/
  include/kos.h           the umbrella header a program includes
  include/kos/            console.h fs.h ipc.h process.h diag.h seg.h xmem.h
  include/klibc/          string.h fmt.h stdarg.h
  src/crt0.asm            startup: the startup block, argv, main, exit
  src/runtime.asm         the fixed low-data area, the shared scratch, the error mapper
  src/gate.asm            exit and getinfo gates
  src/ipcgate.asm         the call gate
  src/nsgate.asm          the bind, unbind, list and exec blocks
  src/diaggate.asm        the diagnostic gates
  src/xmemgate.asm        extended memory and the aperture helpers
  src/sdcc_calls.asm      SDCC's indirect-call trampolines
  src/bcall.asm           ___sdcc_bcall_ehl, the banked-call trampoline
  src/args.c              startup(), splitargs()
  src/ipc.c               call, bind, unbind, list, chdir, getcwd
  src/fs.c                open, read, write, seek, sync, stat, mkdir, ...
  src/exec.c              run()
  src/xmem.c              xalloc and friends
  src/seg.c               seginfo, segmap, farcall, farmap, blob
  src/xload.c             xload()
  src/console_proto.c     the CONSOLE protocol client
  src/console_io.c        console_puts, getchar, console_readline
  src/klibc/              one archive member per function
  src/kos_internal.h      SDK-private glue (not for programs)
  exe.mk                  the in-tree wrapper: sets the in-tree roots, builds
                          libkos from src/, includes mk/kos.mk
  template/               the copyable minimal program (Makefile, hello.c)
  examples/               hello, fardemo, fardata, files

mk/kos.mk                 the program build rule (shipped in the SDK)
user/monitor/             the monitor and its private helpers
user/helloworld/          the demo image's Makefile and README
```

## 2. Include rules

A program compiles with exactly:

```text
sdcc -mz80 -c --nostdinc --Werror -I abi -I user/sdk/include -I <program dir>
```

- `#include "kos.h"` is the only header a program needs. It pulls in
  `kos/console.h`, `kos/fs.h`, `kos/ipc.h`, `kos/process.h`, `kos/seg.h`,
  `kos/xmem.h`, `klibc/string.h` and `klibc/fmt.h`. The diagnostic
  wrappers are not included: a program that needs them includes
  `kos/diag.h` itself, as the monitor does.
- The SDK's `kos/*.h` headers include the matching `abi/kos/*.h` header,
  so each ABI declaration exists once. SDCC resolves a quoted include in
  the including file's directory first, which is how `kos/fs.h` reaches
  `abi/kos/fs.h` and not itself.
- A program may include its own headers, and a test program may add a
  narrow include root with `INCLUDES=`.
- A program never includes `kernel/`, `drivers/`, `formats/` or
  `platform/`: no `-I` is given for them, and `tests/test_sdk.py` resolves
  every quoted include of every program to prove it.
- There is no hosted C library: `--nostdinc` plus `klibc` (section 5). No
  `malloc`, no stdio streams, no floating point, no POSIX emulation.

## 3. The build rule: `exe_program`

```make
include user/sdk/exe.mk          # in the tree; the standalone SDK includes mk/kos.mk
$(eval $(call exe_program,NAME,DIR,SOURCES[,OPTION=VALUE ...]))
```

`SOURCES` are relative to `DIR`; `.c` goes through SDCC, `.asm` through
`sdasz80`. The rule links crt0 and the runtime object, then the program,
then `libkos.lib`; derives the manifest from the link map; and packs and
validates `build/user/NAME.exe`. Intermediates go to `build/user/NAME/`, so
programs build in parallel. Each declaration also defines a `NAME` target.

| option | default | meaning |
|---|---|---|
| `STACK=N` | `256` | the stack reservation |
| `SCRATCH=SYM:SIZE` | `kos_scratch:16` | the scratch symbol the packer must find in `0100h-1FFFh`; `SCRATCH=` declares none |
| `ENTRY=SYM` | `kos_entry` | the entry symbol |
| `NO_CRT0=1` | off | link no crt0; `ENTRY` names the program's own entry |
| `LOWBASE=ADDR` | `0x0100` | base of the SDK's 93-byte fixed low-data area |
| `CODEBASE=ADDR` | `0x015D` | base of code, just after the low-data area |
| `STAGE=1` | off | copy the packed program into the image's module directory so a system file's `store` can name it |
| `INCLUDES=a:b` | empty | extra include roots, for program-owned headers |
| `FAR1=SRC:SRC` … `FAR4=SRC:SRC` | none | far-code sources of segment n, linked at `8000h` (section 6.6) |
| `DATA1=SRC:SRC` … `DATA4=SRC:SRC` | none | far-data sources of data segment n, linked at `8000h` (section 12) |
| `BLOBS=a.bin:b.bin` | none | binary payloads bundled as plain data segments (at most 32 KiB each) |
| `SEGMENTS=FILE` | none | a hand-written JSON segment table, instead of the generated one |

There is no hand-written manifest: `tools/exemanifest.py` computes
`image_size = s__DATA - 0100h`, `bss_size = l__DATA` and
`entry_offset = ENTRY - 0100h` from the map, and `tools/exepack.py`
validates the container against the map's symbols, the scratch window
included.

**Segment rows.** Without `SEGMENTS=`, the rule generates the table: one
far-code row per `FARn=` (ids 1..k), then one mappable data row per
`DATAn=` (ascending), then one plain data row per `BLOBS=` file (in
order). `FARn=` and `DATAn=` options are numbered from 1 without gaps. With
`SEGMENTS=`, the file lists every row and the far-code rows must come
first, because a far segment's id is the number its `__banked` definitions
name.

**The far link rule.** A program with far code or far data keeps its
primary image and BSS below `8000h` and its stack base at or above
`C000h`, so `8000h-BFFFh` belongs to the segments. The packer checks this
and refuses a far area over 16 KiB or not based at `8000h`.

The rule file is a prerequisite of the archive and of every link script,
so editing it rebuilds `libkos` from scratch and relinks every program.

## 4. Startup, arguments and exit

The kernel enters the program at `kos_entry` (crt0, linked first) with
`HL = 0080h`, the startup block (`docs/abi.md` section 5.4):

1. crt0 stores `HL` for `startup()`;
2. it checks `version == 1` and `tail_length <= 125`; anything else is
   treated as an empty command line;
3. it copies the tail into the SDK's own 127-byte buffer and runs
   `splitargs(tail, argv, KOS_ARGV_MAX)` over the copy, in place, so
   `argv` points into program data and survives later syscalls;
4. it calls `main(argc, argv)` with `argc` clamped to `KOS_ARGV_MAX` (16);
5. it calls `exit` with `main`'s return value, and halts if that returns.

`argv` holds the tail's words only: the kernel passes no program name, so
`argv[0]` is the first argument and a program run with no arguments sees
`main(0, argv)`. A program that ignores its arguments may write
`int main(void)`.

**Splitting rules** (runtime policy, not ABI): words are separated by
spaces or tabs; a double quote toggles a quoted section whose separators
are literal and whose quotes are removed; adjacent quoted and unquoted
pieces join (`ab"c d"e` is one word, `abc de`); `""` is an empty argument;
an unterminated quote runs to the end; there is no escape character.

**The boot launcher runs its program again after every exit.** A program
that is the image's boot program and exits normally is started again
immediately, with the launcher's line between runs:

```text
monitor: exit status 0x0000 active 0 free-banks 6
```

(The label is fixed; it does not name the program.) A long run of
identical lines is the evidence that every exit returned the process slot,
the bank, the handles and the CWD.

## 5. `klibc`

One archive member per function, so a program links only what it calls.
Nothing allocates.

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

`printf` supports `%s %c %d %u %x %ld %lu %lx %%` with an optional width
and a `0` pad flag (`%05d`, `%4s`), and writes through `console_write`.
`snprintf` uses the same formatter into a bounded buffer that is always
NUL-terminated. Integer division and modulo helpers (`__divuint`,
`__moduint`, `__divulong`, ...) are provided in `klibc`, built from shifts
and subtraction.

## 6. The `libkos` API

Every wrapper keeps its argument block in the low-window scratch and
returns the result or `-errno`.

### 6.1 Console and process (`kos/console.h`)

```c
void exit(int status);                                  /* does not return */
int  getinfo(struct kos_info_record *record);
int  console_write(const char *buf, unsigned int len);  /* CONSOLE WRITE   */
int  console_read(char *buf, unsigned int len);         /* READ, no wait   */
int  console_poll(void);                                /* POLL            */
int  putchar(int ch);                                   /* PUTC; returns ch */
int  getchar(void);                                     /* waits           */
int  console_puts(const char *text);                    /* no newline added */
int  console_readline(char *buffer, unsigned int capacity);
```

The console functions speak the `CONSOLE` protocol through the standard
handle; a process without one gets `-KOS_EINVAL`. `console_readline` edits
one line (backspace, DEL, CR/LF, a bell on overflow; `KOS_LINE_MAX` is 80);
it is the monitor's input loop.

### 6.2 Files (`kos/fs.h`)

```c
int  open(const char *path, unsigned char flags);   /* bind + FILE check */
int  close(unsigned int handle);                    /* sync + unbind     */
int  read(unsigned int handle, void *buffer, unsigned int length);
int  write(unsigned int handle, const void *buffer, unsigned int length);
long seek(unsigned int handle, long offset, unsigned char whence);
int  sync(unsigned int handle);
int  stat(const char *path, struct kfs_stat *out);
int  mkdir(const char *path);
int  rmdir(const char *path);
int  unlink(const char *path);
int  rename(const char *oldpath, const char *newpath);
```

Paths are at most 127 bytes; a longer or empty one is refused locally with
`-KOS_EINVAL`. `docs/filesystem.md` has the semantics.

### 6.3 Objects and the namespace (`kos/ipc.h`)

```c
int bind(const char *path, unsigned int flags, unsigned char *type);
int call(unsigned char handle, struct kos_ipc_request *req);
int unbind(unsigned char handle);
int list(const char *path, unsigned int *cookie,
         struct kos_ns_entry *out, unsigned int max_entries);
int chdir(const char *path);
int getcwd(char *buffer, unsigned int length);
```

```c
struct kos_ipc_request {
	struct kos_ipc_frame frame;   /* 32 bytes, abi/kos/ipc.h  */
	void *buffer;                 /* the call's one buffer    */
	unsigned int length;          /* 0 = no buffer            */
	unsigned char direction;      /* KOS_IPC_DESC_IN or _OUT  */
};
```

`call` returns the IPC status: the driver's result, or a negated `KOS_E*`
refusal; a request that never reached the IPC layer is mapped to `-errno`
too, so a driver error and a malformed request are never confused. The
buffer may be anywhere in `0100h-DFFFh` or in common memory; the nucleus
picks the copy path. Standard handles 1 and 2 exist before the first
`bind`.

### 6.4 Processes (`kos/process.h`)

```c
#define KOS_ARGV_MAX 16
struct kos_startup *startup(void);
int  splitargs(char *tail, char **argv, int max);
int  run(const char *path, const char *args);
```

`run` runs a child synchronously with a raw tail (NULL or empty for none)
and returns the child's exit status or `-errno`. A path over 127 bytes or a
tail over 125 is refused locally.

### 6.5 Extended memory (`kos/xmem.h`)

```c
int xalloc(unsigned int size, unsigned int flags);
int xfree(unsigned int handle);
int xread(unsigned int handle, unsigned int offset, void *buffer,
          unsigned int length);
int xwrite(unsigned int handle, unsigned int offset,
           const void *buffer, unsigned int length);
int xinfo(unsigned int handle, struct kos_xmem_info *out);
int xmap(unsigned int handle, unsigned int *address);
unsigned char aperture_open(unsigned char token);   /* returns the previous latch */
void aperture_close(unsigned char previous);
```

`aperture_open` and `aperture_close` are register-only: no syscall.

### 6.6 Segments and far code (`kos/seg.h`)

```c
int  seginfo(unsigned int segment, struct kos_seg_info *out);
int  segmap(unsigned int segment);
typedef int (*kos_far_fn)(int, int);
int  farcall(unsigned int segment, kos_far_fn entry, int a, int b);
void *farmap(unsigned int n, unsigned char *prev);
void farunmap(unsigned char prev);
int  blob(unsigned int n);
int  xload(const char *path, unsigned int flags);
#define KOS_RT_EFARMAP 0x30
```

**Banked calls.** A function defined `__banked` in a `FARn=` source is
called as plain C:

```c
extern int far_main(int a, int b) __banked;   /* defined in a FAR1= source */
result = far_main(a, b);
```

SDCC emits `ld e,#b_<fn>` / `ld hl,#_<fn>` / `call ___sdcc_bcall_ehl`;
the SDK's trampoline (`src/bcall.asm`) keeps one token per segment (only
the first call of a segment costs a `SYS_SEG_MAP`), opens the aperture,
calls, and restores the caller's latch. Arguments travel on the stack;
results come back in `A`, `DE` or `DEHL`. Far code may make banked calls of
its own. A banked call that names no far segment reports `KOS_RT_EFARMAP`
(an SDK-level code outside the syscall error range) and does not run.

**`farcall`** is the primitive under it: it maps a segment, calls a far
entry by its linked address and restores the latch. Far code may call
primary code directly and make syscalls, whose buffers follow the
buffer-identity rule while the aperture is open.

`farmap`, `farunmap`, `blob` and `xload` are section 12.

### 6.7 Diagnostics (`kos/diag.h`)

`diag_mem_read`, `diag_mem_write`, `diag_io_in`, `diag_io_out`,
`diag_bank_info` and `diag_system_info`, with their blocks in
`abi/kos/diag.h`. Recovery tools only; `kos.h` does not include them.

## 7. Memory layout

```text
0100h-015Ch     the SDK's fixed low data (_KOS_SDK_DATA, 93 bytes: the
                scratch, the call, namespace and exec blocks)
015Dh           crt0 (kos_entry)
...             the program's code
0100h+image     BSS: crt0's tail copy and argv, then the program's statics
...             free memory
E000h-STACK     the stack, growing down from E000h
0100h-1FFFh     the syscall argument window: every gate-visible block is here
8000h-BFFFh     with the aperture open: a far segment or a mapped object
```

The runtime object is linked into every program and pins `_KOS_SDK_DATA`
before `_CODE`, so the gate-visible blocks stay in the argument window
however large a program's BSS grows. A program with far code or far data
keeps `8000h-BFFFh` free (the far link rule).

A program linked with `NO_CRT0=1` still gets the low-data area. The IPC
acceptance probe sets `LOWBASE=0x0300 CODEBASE=0x0344` to leave
`0100h-02FFh` as a plain 512-byte buffer.

## 8. What links what

`libkos` is an archive: a member is linked only when a symbol it defines is
needed.

| a program calls | members linked |
|---|---|
| anything | `kos_crt0`, `kos_runtime`, `args` |
| `console_write`, `console_read`, `console_poll`, `putchar` | `console_proto`, `ipc`, `kos_ipcgate`, `nsgate` |
| `console_puts`, `getchar`, `console_readline` | `console_io`, plus the above |
| `exit`, `getinfo` | `kos_gate` |
| `open`, `read`, ... | `fs`, `ipc`, `kos_ipcgate`, `nsgate` |
| `bind`, `call`, `unbind`, `list`, `chdir`, `getcwd` | `ipc`, `kos_ipcgate`, `nsgate` |
| `run` | `exec`, `ipc`, `kos_ipcgate`, `nsgate` |
| `xalloc` and friends | `xmem`, `kos_xmemgate` |
| `seginfo`, `segmap`, `farcall`, `farmap`, `blob` | `seg` |
| `xload` | `xload` |
| a `__banked` call | `kos_bcall` |
| an indirect call SDCC cannot bind at link time | `kos_sdcc_calls` |
| `diag_*` | `kos_diaggate` |
| `printf` | `printf`, `strlen`, `utoa`, `ultoa`, `itoa`, `ltoa` |
| `/` or `%` on integers | `sdcc_arith` / `sdcc_arith_signed` and their entry stubs |
| `strcmp`, `memset`, ... | that one member |

`hello.exe` links exactly crt0, the runtime, `args`, the console client
with its gates, `printf` with its formatters and the arithmetic helpers,
and no filesystem, exec or diagnostic member; `tests/test_sdk.py` checks
the list.

## 9. Tests

`tests/test_sdk.py` and `tests/test_source_layout.py`:

- every program's quoted includes resolve inside its directory,
  `user/sdk/include`, `abi` and its `INCLUDES=`, never into the OS tree,
  far sources included;
- the compile flags are exactly the ones in section 2, and far sources get
  their bank and segment flags;
- `exepack` refuses a far area that is not at `8000h`, over 16 KiB, or a
  program that reaches into `8000h-BFFFh`;
- `libkos` has one member per source and no kernel object;
- `hello.exe`'s manifest matches its map, the container validates, the
  scratch is in the window, and the linked members are exactly the
  expected set;
- the simulator runs `hello.exe`'s bytes: `splitargs` turns a quoted tail
  into the documented `argv`.

`tests/test_bcall.py` checks the banked-call convention on a real SDCC
build and runs the trampoline in the host Z80. `tests/test_far_data.py`
checks the far-data rules and runs the far-data program.
`tests/test_sdk_package.py` checks the standalone SDK (section 11).

## 10. The demo image and the examples

`user/helloworld/Makefile` builds the examples, and

```sh
make helloworld-image
```

builds a separate tree (`build-helloworld/`) from `systems/helloworld.json`,
whose boot program is `/system/hello.exe`. Nothing else differs from
production. On a board the transcript is the repeating pair

```text
Hello, KonaOS!
monitor: exit status 0x0000 active 0 free-banks 6
```

because the launcher runs the program again after each exit (section 4).

The examples:

- **`hello`**: prints a greeting and its arguments.
- **`fardemo`** (`fardemo.c`, `fardemo_far.c` as `FAR1`): the smallest
  far-code program. The primary half maps its segment with `segmap` and
  calls the `__banked` far entry as plain C; the far half prints from a
  literal inside the aperture, calls back into primary code and returns
  `(latch << 8) | value`. Booted as the boot program it prints:

  ```text
  fardemo: begin
  fardemo: token 50
  fardemo: hello from the far bank
  fardemo: far result 502B
  fardemo: PASS
  ```

  The token is the aperture latch for the bank the loader chose (bank 5,
  so P = 2).
- **`fardata`** (`fardata.c`, `levels.c` as `DATA1`, `fardata_far.c` as
  `FAR1`): the smallest far-data program. It maps the data segment, reads a
  4 KiB table and writes a zeroed 2 KiB array, unmaps, and only then makes
  a banked call with values copied out of the table: the one-window rule in
  one program.
- **`files`**: opens, reads and lists files.

## 11. The standalone SDK

```sh
make sdk        # build/kos-sdk-<version>/ and its .tar.gz
```

The package holds everything a program needs and nothing from the tree it
was built in. Repacking unchanged sources reproduces it byte for byte (the
library is archived with zeroed dates and ids, and the tarball pins its
order, times and ownership).

```text
kos-sdk-<version>/
  VERSION          "<version> abi <n>", n = the loader's EXE_ABI_VERSION
  include/         kos.h, kos/*.h, klibc/*.h, abi/kos/*.h
  lib/             libkos.lib (the runtime object included), crt0.rel
  tools/           exepack.py, exemanifest.py, ihx.py (Python standard library only)
  mk/kos.mk        the program rule
  template/        Makefile and hello.c
  examples/        hello, fardemo, fardata, files
  docs/            sdk.md, exe-format.md
```

The ABI headers sit under `include/abi/kos/` and the compile line keeps the
tree's order (`-I include/abi -I include`), so every header resolves the
paths its includes name.

From unpacking to running:

1. unpack the tarball and `cd` into `template/` (or any directory whose
   Makefile includes the SDK's `mk/kos.mk` and sets `KOS_SDK_ROOT`);
2. `make`: the rule builds `build/user/<name>.exe` from `lib/`;
3. copy the `.exe` to the card (`/fs`), as an 8.3 name;
4. run it from the monitor with `X /fs/<name>.exe`.

A new program is a copy of `template/` with its name in the `exe_program`
line. A program built for another ABI version is refused by the loader.

`tests/test_sdk_package.py` builds the tarball, unpacks it outside the
repository, checks the file list, the header closure, the version and the
archive's metadata, builds `template/` and `examples/` with the tree's
paths absent, runs the result through the production image's monitor in
the simulator, and checks that a copy with a changed ABI byte is refused.

## 12. Far data

Data that does not fit in the program's 56 KiB. Four ways in:

| kind | declared | reached | costs |
|---|---|---|---|
| `DATAn=` sources | at build time, in C | `farmap(n)`: the sources' symbols work as plain pointers until `farunmap` | one auxiliary bank per segment, for the program's whole life |
| `BLOBS=` files | at build time, binary files | `blob(n)` gives the handle; `xread`/`xwrite` copy | slots only; a copy per access |
| `xalloc` | at run time | `xread`/`xwrite`, or `xmap` + `aperture_open` if mappable | a bank's aperture slice for a mappable object, slots otherwise |
| `xload(path, flags)` | at run time, a file | its handle, as for `xalloc` | one copy of the file |

```c
unsigned char saved;
const unsigned char *table = farmap(1, &saved);   /* the DATA1 segment */

if (table == (const unsigned char *)0) {
	return 1;        /* `saved` holds the negated error */
}
level = table[level_index];   /* a plain access */
farunmap(saved);              /* restore the caller's mapping */
```

- A `DATAn` source holds `const` tables and zeroed objects. The `const`
  objects form the segment's image at `8000h` (`--constseg DATA<n>`); the
  zeroed ones its BSS right after (`--dataseg DATA<n>B`), which the loader
  zeroes. An initialized writable object is refused at build time with its
  name: initialize it at run time.
- `xload` loads a whole file: at most 32 KiB, or 16 KiB when mappable. A
  mappable object is filled by reading straight into the open aperture
  (one copy); a plain one through a 512-byte buffer. A larger file is
  streamed with `open`/`read` instead.

**One window.** There is one far window, and far code shares it with
mapped far data: a `__banked` function and a mapped segment are never
visible together. Primary code maps data and uses pointers; a `__banked`
function reaches far data only by copy (`xread`, or values the primary
copies out first). `farunmap` before a banked call and `farmap` after: each
mapper restores exactly the latch it saved.

**Bank budget.** Each far-code segment and each mappable data segment takes
one auxiliary bank's aperture slice; the program's own bank never counts.
With six application banks, at most five such segments fit, and fewer when
other processes hold banks. A program that asks for more is refused when it
is run (`run` returns `-KOS_ENOSPC`) and nothing is allocated.

**Costs.** A banked call costs about 31 µs. Access through an open window
costs nothing extra. `xread` costs about 21 T-states per byte in the fast
slots and 287 in the slow ones. Walk a large table by mapping it; read a
few bytes from many places by copying.
