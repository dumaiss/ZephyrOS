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
  include/kos/            console.h fs.h ipc.h process.h diag.h seg.h
                          xmem.h timer.h sound.h gfx.h
  include/klibc/          string.h fmt.h stdarg.h
  src/crt0.asm            startup: the startup block, argv, main, exit
  src/runtime.asm         the fixed low-data area, the shared scratch, the error mapper
  src/gate.asm            exit, getinfo and reboot gates
  src/ipcgate.asm         the call gate
  src/nsgate.asm          the bind, unbind, list and exec blocks
  src/diaggate.asm        the diagnostic gates
  src/xmemgate.asm        extended memory and the aperture helpers
  src/sdcc_calls.asm      SDCC's indirect-call trampolines
  src/bcall.asm           ___sdcc_bcall_ehl, the banked-call trampoline
  src/args.c              startup(), splitargs()
  src/ipc.c               call, bind, unbind, list, chdir, getcwd
  src/fs.c                open, read, write, seek, sync, stat, mkdir, ...
  src/truncate.c          truncate
  src/exec.c              run()
  src/xmem.c              xalloc and friends
  src/seg.c               seginfo, segmap, farcall, farmap, blob
  src/xload.c             xload()
  src/console_proto.c     the CONSOLE protocol client
  src/console_io.c        console_puts, getchar, console_readline
  src/timer.c             the TIMER protocol client: timer_open, timer_now,
                          timer_us, timer_wait_us
  src/klibc/              one archive member per function
  src/kos_internal.h      SDK-private glue (not for programs)
  exe.mk                  the in-tree wrapper: sets the in-tree roots, builds
                          libkos from src/, includes mk/kos.mk
  tui/                    the TUI SDK: libtui and libtui_widgets (tui.md)
  widgets/                the widget layer's sources (part of the TUI SDK)
  gfx/                    the graphics SDK: libgfx (gfx-sdk.md)
    src/gfx.c             gfx_open, gfx_close, gfx_get_info, the state
    src/gfx_mode.c        gfx_set_mode, gfx_display_enable
    src/gfx_draw.c        gfx_clear, gfx_fill_rect, gfx_copy_rect
    src/gfx_shapes.c      gfx_rect/line/point/ellipse/fill_ellipse
    src/gfx_batch.c       gfx_begin/end/flush, the command queue
    src/gfx_upload.c      gfx_upload_begin/write/end (pixel streaming)
    src/gfx_palette.c     gfx_set_palette
    src/gfx_font.c        the 6x8 RAM font and its glyph bodies
    src/gfx_font4.c       the 4x6 RAM font, the 32-byte pair tables,
                          the glyph calls' dispatch (font + stale marks)
    src/gfx_text.c        gfx_text/text_n/text_width, the row loop
    src/gfx_atlas.c       gfx_text_atlas, the transparent path
    src/backends/v9958/   the V9958 backend: mode/command C and port I/O
  template/               the copyable minimal program (Makefile, hello.c)
  examples/               hello, fardemo, fardata, files

mk/kos.mk                 the program build rule (shipped in the SDK)
user/monitor/             the monitor and its private helpers
user/shell/               the native shell (main, parse, path, lookup,
                          builtin, fscmd, banner, logo - the banner's
                          logo lives in logo.c, generated from
                          konaos-logo-80x46.lmmc by tools/g6pack.py)
user/helloworld/          the demo image's Makefile and README
```

## 2. Include rules

A program compiles with exactly:

```text
sdcc -mz80 -c --nostdinc --Werror -I abi -I user/sdk/include -I <program dir>
```

- `#include "kos.h"` is the only header a program needs. It pulls in
  `kos/console.h`, `kos/fs.h`, `kos/ipc.h`, `kos/process.h`, `kos/seg.h`,
  `kos/xmem.h`, `klibc/string.h`, `klibc/fmt.h` and `klibc/stdio.h`. The
  diagnostic wrappers are not included: a program that needs them includes
  `kos/diag.h` itself, as the monitor does. `kos/stream.h` is the same
  deal for the stream measurement and planning calls (section 6.10).
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
then any `LIBS=` archives in their declared order, then `libkos.lib`;
derives the manifest from the link map; and packs and validates
`build/user/NAME.exe`. Intermediates go to `build/user/NAME/`, so programs
build in parallel. Each declaration also defines a `NAME` target.

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
| `LIBS=a:b` | empty | optional SDK libraries in link order; each resolves to `lib/lib<name>.lib` (or the corresponding in-tree archive) |
| `TUI_GRID=CxR` | `85x26` | the libtui capacity: `tui` in `LIBS` links `lib/libtui_<C>x<R>.lib` (`128x35`), or `libtui.lib` for `85x26`; any other grid is a build error |
| `FAR1=SRC:SRC` … `FAR4=SRC:SRC` | none | far-code sources of segment n, linked at `8000h` (section 6.6) |
| `DATA1=SRC:SRC` … `DATA4=SRC:SRC` | none | far-data sources of data segment n, linked at `8000h` (section 12) |
| `BLOBS=a.bin:b.bin` | none | binary payloads bundled as plain data segments (at most 32 KiB each) |
| `SEGMENTS=FILE` | none | a hand-written JSON segment table, instead of the generated one |

The TUI libraries are optional SDK archives: `LIBS=tui` links the core
`libtui`, and `LIBS=tui_widgets:tui` links the procedural widget archive
before its core dependency.  Include `tui.h` for the core and
`tui_widgets.h` for widgets.  The application guide is
[tui.md](tui.md); a program that uses neither library pays no TUI code or
data cost.

The graphics SDK is the same deal: `LIBS=gfx` links `libgfx`, whose
program includes `kos/gfx.h` and calls `gfx_open` to take `/dev/video`
exclusively.  The design is [gfx-sdk.md](gfx-sdk.md); a program that
uses no graphics pays no gfx code or data cost.

The drawing pieces hang off the same library.  `gfx_clear`,
`gfx_fill_rect` and `gfx_copy_rect` are the rectangle primitives
(HMMV/HMMM; a copy chooses its direction from the rectangles'
overlap, and source or destination may sit on the off-screen page);
`gfx_begin`/`gfx_end`/`gfx_flush` batch them into a process-local
queue of 32 records, flushed in order when full, and a call outside a
batch executes at once.  `gfx_upload_begin`,
`gfx_upload_write` and `gfx_upload_end` stream a rectangle's pixels
straight to VRAM through the data port (any chunk length, one address
per row, the exact byte count enforced), and `gfx_set_palette`
programs the 16-colour mode's palette.  `gfx_text` and `gfx_text_n`
draw a string with a style: the colours, a `font` (the built-in 6x8
CP850, or the 4x6 that fits 128 columns in the 16-colour mode) and a
`flags` word; a zeroed style is opaque 6x8.  Opaque text is
scanline-major in assembly: one VRAM address per scanline of the
string, then a row loop streams every glyph's bytes (the font row
byte, the pair-table lookup, OUT) - in the 16-colour mode the 6x8
expansion comes from a cached three-page pair table and the 4x6's
from a 32-byte table per pair, in the 256-colour mode one byte per
pixel - and `gfx_text_width` measures a string in the font's pixels
(6 or 4 per glyph).  `GFX_TEXT_TRANSPARENT` draws only each glyph's
foreground: `gfx_text_atlas` hands libgfx an off-screen band, a font
and a glyph range, and each glyph is one batched LMMM with
transparency (TIMP) from an atlas of its colour, so colour-0 pixels
leave the screen untouched.  The band must lie whole in one page's
spare lines (212-255 or 468-511) and hold one atlas; an atlas is
built on its colour's first transparent use and rebuilt least
recently used when the band is full, and a glyph redefined since the
atlas was built is redrawn into every atlas at its next use, alone.
`gfx_font_set_glyph(font, code, rows)` installs a glyph (the low six
bits of each 6x8 row, the low four of each 4x6 row) into the font's
RAM copy, `gfx_font_reset_glyph` restores one glyph and
`gfx_font_reset` restores them all.  The 6x8 glyph calls live in
gfx_font.c, the dispatch in gfx_font4.c, so a 6x8-only program links
no 4x6 member and a program that never uses transparent text links
no atlas member; the atlas build uses the same opaque emit, so it
adds no second expansion.  Measured on the emulated 10 MHz CPU in
T-states (the simulator's V9958 peer timestamps every register and
status access; the figures are pinned by tests/test_gfx.py): a full
128-glyph 4x6 row costs ~286,000 T-states with a fresh colour pair
(~2,200 per glyph) and ~234,000 cached, an opaque 6x8 glyph in a
cached pair ~6,900, an opaque 4x6 glyph in the 256-colour mode
~4,200, a transparent glyph ~10,000 of CPU plus its LMMM, a 95-glyph
atlas build ~2,330,000 - a once-per-colour cost - and the stale
redraw of a redefined glyph into every atlas ~211,000.  The shapes of gfx-sdk.md section
8.1 take an `enum gfx_op` (COPY or XOR) each: `gfx_rect` is four LINE
commands with each corner plotted exactly once, `gfx_line` and
`gfx_point` are one LINE or PSET from the endpoint coordinates (the
command's major/minor axes and direction bits derived from them),
`gfx_fill_ellipse` and `gfx_ellipse` expand the midpoint ellipse into
one horizontal span (LMMV) per scanline - outline ellipses as
non-overlapping left/right spans, so an XOR ellipse drawn twice
restores everything - and `gfx_fill_rect` already takes the op, with
HMMV for an aligned COPY and the per-pixel LMMV otherwise.  The
ellipse walk runs on the CPU; the spans queue like any other record
and the batch auto-flushes as it fills.  In YJK only an aligned COPY
`gfx_fill_rect` is accepted; the other shapes are -KOS_ENOTSUP.

Drawing is asynchronous: the backend waits for the VDP's command
engine before it starts each command, not after, so `gfx_end` (and a
call outside a batch) returns with the VDP still drawing and the CPU
runs while it finishes; every CPU VRAM path - the upload, the text
stream, an atlas build, the palette, a mode set, a page switch,
`gfx_close` - waits for the engine first, so it can never race a
running command.  `gfx_wait` waits for the last command to finish, for a program that reads VRAM
or times its frame.  Both drawing modes hold two 256-line pages, and
`gfx_show_page(0|1)` switches the displayed page at the next vertical
blank (it waits for the engine and the retrace, then writes R#2 -
atomic, no tearing; `gfx_get_info` reports the shown page);
`gfx_wait_vblank` waits for the start of the next retrace, for a
frame loop or a partial present: draw a band on the hidden page
(y + 256 for page 1), then `gfx_wait_vblank()` and `gfx_copy_rect`
it onto the shown page - the VDP's copy speed decides how much of the
band lands inside the ~3.2 ms blank.
Pictures come prepared: the
KIMG v1 format (`kos/kimg.h`) carries a header, an optional palette and
the payload in the mode's packing, `tools/kimg.py` converts
PNG/JPEG to it on the host, and `pview.exe` (UserSoftware/PView)
displays a picture file; the design is gfx-sdk.md sections 10-17.
`gfxdemo.exe` (UserSoftware/GfxDemo) is the whole surface in one
picture - clear, bars, copied fills, text in two colour pairs and an
application glyph drawn, restored and drawn again (gfx-sdk.md
section 23).
The packings are the V9958's own:

- **16-colour (G6):** two palette indices per byte, the left pixel in
  the high nibble.
- **256-colour (G7):** one byte per pixel, green in bits 7-5, red in
  4-2, blue in 1-0.
- **YJK:** one byte per pixel, the pixel's 5-bit luminance Y in bits
  7-3.
  - **Shared colour:** each run of four pixels (x a multiple of 4)
    shares two signed 6-bit colour differences in bits 2-0: K in the
    first byte (low bits) and the second (high), J in the third (low)
    and the fourth (high).
  - **Display:** the chip shows R = Y + J, G = Y + K and
    B = (5Y - 2J - K) / 4, clamped to 0-31.

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
spaces or tabs; a single quote toggles a literal section; a double quote
toggles a quoted section whose separators are literal and inside which
`\"` and `\\` are escapes; outside both quotes `\x` is a literal `x`;
adjacent pieces join (`ab"c d"e` is one word, `abc de`); `''` and `""`
are empty arguments; an unterminated quote runs to the end.

**The boot launcher's policy follows the image's system file.** A program
that is the image's boot program and exits normally gets one of two
treatments, and the launcher's line precedes each:

```text
boot: /system/shell.exe exit 0        (production: started again)
boot: /system/hello.exe exit 0        (an acceptance image: the monitor follows)
```

The line names the boot program and its status; a run that cannot start
prints `boot: <path> failed <n>` and falls back to the monitor instead. In
every case an exit returns the process slot, the bank, the handles and the
CWD before the next program starts.

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
and a `0` pad flag (`%05d`, `%4s`), and writes through `putchar` - so its
output is line-buffered with everything else on the console (section
6.1). `snprintf` uses the same formatter into a bounded buffer that is
always NUL-terminated. Integer division and modulo helpers (`__divuint`,
`__moduint`, `__divulong`, ...) are provided in `klibc`, built from shifts
and subtraction.

`klibc/stdio.h` is the buffered file layer (an ISO C subset; no
allocation):

```c
typedef struct kos_stream FILE;            /* four streams, one pool     */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)
FILE  *fopen(const char *path, const char *mode);   /* "r" "w" "a", 'b' ok */
int    fclose(FILE *stream);                        /* flush + close       */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int    fgetc(FILE *stream);   int getc(FILE *stream);
int    fputc(int c, FILE *stream);   int putc(int c, FILE *stream);
char  *fgets(char *dst, int size, FILE *stream);
int    fputs(const char *src, FILE *stream);
int    fflush(FILE *stream);                        /* NULL: everything    */
int    feof(FILE *stream);    int ferror(FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);   void rewind(FILE *stream);
```

`fopen` takes one of the four streams (`NULL` when the pool is full or
the open failed); each carries its own 512-byte buffer, so a read is one
card read per 512 bytes and a write leaves when the buffer fills, at
`fclose`, at `fflush` and at exit. A read or write of a buffer or more
bypasses the buffer and moves the bytes once, straight into or out of the
caller's memory. `fseek` flushes pending writes and drops the read
buffer, so the next read starts where it says; `ftell` is the logical
position, counting the bytes still buffered either way. `feof`/`ferror`
report the stream's state, and a call that cannot report an error through
its ISO result returns the negated native code.

A program that never calls `fopen` links none of these members; one that
does gets the exit flush for free, because `exit` calls the console
flush and that calls back into the stream layer when it is linked
(section 8).

One trap: the runtime provides *division* helpers, not a multiply. SDCC
turns a variable or 32-bit product into a call to `__mulint`/`__mulong`,
which no member defines, so such a program does not link. Products of a
value and a small constant fit only when the low byte matters or the
constant is a power of two (a shift); otherwise write the shift-and-add
loop (the stream layer's `stream_plan` does, section 6.10).

## 6. The `libkos` API

Every wrapper keeps its argument block in the low-window scratch and
returns the result or `-errno`.

### 6.1 Console and process (`kos/console.h`)

```c
void exit(int status);                                  /* does not return */
void reboot(void);                                      /* does not return */
int  getinfo(struct kos_info_record *record);
int  console_write(const char *buf, unsigned int len);  /* CONSOLE WRITE   */
int  console_render(const void *stream, unsigned int len); /* one render txn */
int  console_info(struct kconsole_info *out);           /* grid, capabilities */
int  console_read(char *buf, unsigned int len);         /* READ, no wait   */
int  console_poll(void);                                /* POLL            */
int  putchar(int ch);                                   /* buffered; returns ch */
int  console_flush(void);                               /* write it out now */
int  getchar(void);                                     /* waits           */
int  console_puts(const char *text);                    /* no newline added */
int  console_readline(char *buffer, unsigned int capacity);
```

The console functions speak the `CONSOLE` protocol through the standard
handle; a process without one gets `-KOS_EINVAL`. `console_readline` edits
one line (backspace, DEL, CR/LF, a bell on overflow; `KOS_LINE_MAX` is 80);
it is the monitor's input loop.

`console_render` is the low-level entry used by `libtui`; its versioned byte
layout is in `kos/console_render.h`. It flushes buffered console bytes first,
then submits exactly one descriptor. A backend without the extension returns
`-KOS_ENOTSUP`. `console_info` returns the console's text grid and
capabilities (`struct kconsole_info`, also in `kos/console_render.h`):
columns, rows, the attribute bits a render record may use and whether
RENDER is served. Render streams contain absolute TEXT spans, in-place INVERT
spans, cursor/attribute STATE, and a full-width one-row SCROLL region; every
record is validated against the reported grid before the backend changes it.

Output is line-buffered: `putchar`, `console_puts` and `printf` collect
into a 128-byte per-program buffer, and it is written out when it fills,
at a `'\n'`, before every console read (`getchar`, `console_read`,
`console_poll`, `console_readline`), before `run`, and at exit. So a
prompt written without a newline still arrives before the input it asks
for, and a child started by `run` never prints ahead of its parent's
pending bytes. `console_write` of more than the free space flushes first
and then sends its bytes directly (one call, not one per buffer). The
error of a write that a flush made is returned by the call that flushed:
`console_write` and `console_flush` return it, `putchar` keeps returning
its byte. A program that faults loses the bytes it has not flushed;
`console_flush` sends them on demand, and `fflush(NULL)` flushes both the
console and every open stream (section 6.10).

### 6.2 Files (`kos/fs.h`)

```c
int  open(const char *path, unsigned char flags);   /* bind + FILE check */
int  close(unsigned int handle);                    /* sync + unbind     */
int  read(unsigned int handle, void *buffer, unsigned int length);
int  write(unsigned int handle, const void *buffer, unsigned int length);
long seek(unsigned int handle, long offset, unsigned char whence);
int  sync(unsigned int handle);
int  truncate(unsigned int handle, unsigned long length);
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
void idle(void);
```

`run` runs a child synchronously with a raw tail (NULL or empty for none)
and returns the child's exit status or `-errno`. A path over 127 bytes or a
tail over 125 is refused locally.

`idle` waits for the next interrupt (`HALT`). A program waiting on input
calls it between empty polls, as `getchar` (and with it
`console_readline`) does:
- **Why:** the program then polls once per interrupt (the timer's tick at
  least, 156.25 Hz) with the CPU halted in between, instead of spinning
  through system calls. A spinning loop's bus activity falls in the audio
  band, and the sound card picks it up.
- **Events:** they are delivered at the interrupt's return as usual, so
  music driven by timer events keeps playing.
- **When it halts:** only where an interrupt is sure to come, that is
  where the system has the timer (`/dev/timer`, asked once) and
  interrupts are enabled. Otherwise it returns at once.

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

### 6.8 The monotonic clock (`kos/timer.h`)

```c
int  timer_open(void);                       /* bind /dev/timer          */
int  timer_now(int handle, uint32_t *now);   /* 25.6 µs counts           */
uint32_t timer_us(uint32_t counts);          /* counts scaled to µs      */
int  timer_wait_us(int handle, uint32_t us); /* polling wait, bounded    */
```

The clock wraps after about 30.5 hours; elapsed times are unsigned
differences. `timer_us` is exact to the count and valid for elapsed times
under about 71 minutes (its largest partial product is 32 bits).
`timer_wait_us` polls `now`, so its resolution is one call. It returns 0
when the wait was served, the negated error when a reading failed, and
`-KOS_ETIMEDOUT` when the clock stops: a reading has to move by one tick
(250 counts) within 100,000 consecutive readings, so a dead or frozen
clock never hangs the caller. A build without the timer driver answers
`-KOS_ENOENT` or `-KOS_ENODEV` from `timer_open`.

### 6.9 Device events (`kos/event.h`)

```c
typedef void (*kos_event_handler_t)(unsigned int count,
                                    unsigned int missed);
int notify(int handle, kos_event_handler_t handler);
int timer_event(int handle, uint32_t period_counts);
```

`notify` subscribes `handler` to the events of a bound device handle; a
second call replaces the handler, and `notify(handle, 0)` unsubscribes
(`docs/abi.md` section 6.7 has the whole contract). `timer_event` arms
the timer driver's event timer with a period in 25.6 µs counts - 782 is
20 ms - and 0 stops it. A handle that holds `/dev/video` exclusively
(`KOS_BIND_EXCLUSIVE`) receives the V9958's frame interrupt instead: the
program enables it (IE0, R#1 bit 5) and gets one event per frame
(`docs/devices.md`, section 6). Both return 0 or a negated error; a handle that
is not bound to an event-serving endpoint is `-KOS_ENOTSUP`, and the
four subscription slots being taken is `-KOS_ENOSPC`.

The handler runs on the program's own stack, not on an interrupt stack,
and it runs *instead of* the interrupted code: what it leaves on the
stack stays there when the interrupted code resumes. It may call the
SDK - that is what a handler's own console write or clock read is - but
each call and each delivery is one more frame below the 24-byte delivery
frame, so the stack reservation (`EXE_STACK` bytes, 384 by default) has
to carry the program's own depth plus the handler's. It must not return
through a setjmp-style escape or long-jump out: the nucleus restores the
state it suspended when the handler returns normally, and a handler that
never returns leaves the delivery open.

Events coalesce: `count` is how many arrived since the last delivery and
`missed` how many were dropped (a child was running, or the subscription
went away), both saturating at `FFFFh`, and both are *since the last
delivery*: a program that wants a running total adds them up.  Nothing
queues, so a handler that is slower than its period simply sees a larger
`count`, and reading the clock in the handler is exact to the count - the
delivery point is the only delay.

The frame is built below the interrupted stack pointer and the handler
runs below it, so the program's stack must not lie in the `8000h-BFFFh`
aperture (the far link rule and the SDK's stack reservation already keep
it out).  The trampoline and `EVENT_RETURN` live in `libkos`, which every
program links; a program never writes its own.

The trampoline also saves the SDK's fixed low argument-block area
(`_KOS_SDK_DATA`, 93 bytes, section 7) into a per-program `_DATA` buffer
before it calls the handler and restores it after.  Every SDK wrapper
stages its request there and then does `RST 30h`, and that staging is
exactly what a delivery can land on: without the snapshot a handler that
calls the SDK would clobber the interrupted call's staged request - or
the reply it is about to read.  Two 93-byte LDIRs per delivery, about
1,970 T-states each, or 2% of the CPU at a 50 Hz period.

### 6.10 Streams: measurement, planning and batching (`kos/stream.h`)

A stream never needs the clock. `fmeasure(stream, timer)` opts one in;
from then on every refill (the read that fills its buffer) is timed with
the TIMER driver, and `fstats(stream, &stats)` reports the set: how many
refills, and the fastest, mean and slowest in 25.6 µs counts.

`stream_plan` turns that - or a figure the caller brings, such as a
card's measured block time - into what a consumer needs to read a file
without starving:

```c
struct kos_stream_need {            /* in */
    unsigned long rate;  /* B, bytes per second the consumer needs */
    unsigned int period; /* T, the period it must be served within, counts */
    unsigned int chunk;  /* C, the refill size, bytes (512) */
    unsigned int mean;   /* t_mean, mean refill time, counts */
    unsigned int worst;  /* t_worst, worst-case refill time, counts */
    unsigned int safety; /* factor for the pre-fetch; 0 selects 2 */
};
struct kos_stream_plan {            /* out */
    unsigned long bytes_per_tick_millis;  /* b = B*T, in milli-bytes */
    unsigned int  ticks_per_refill;       /* n = floor(C / b) */
    unsigned long prefetch;               /* P = safety*B*t_worst + C */
    unsigned long startup_ms;             /* P / R, R = C / t_mean */
    unsigned int  utilisation_pct;        /* U = B / R, in percent */
    unsigned char feasible;               /* U < 50 % */
};
int fmeasure(FILE *stream, int timer);
int fstats(FILE *stream, struct kos_stream_stats *stats);
int stream_plan(const struct kos_stream_need *need,
                struct kos_stream_plan *plan);
```

Read the plan as: eat `n` ticks' worth of bytes, then refill in one
`C`-byte read, right after the tick - a refill that runs longer than `T`
makes the next tick late, never lost. `P` is what has to be in hand
before the consumer starts, so the *worst* refill never starves it, and
`startup_ms` is how long getting `P` together takes at the measured mean
rate. A plan that is not feasible is not an error: it says this medium
cannot serve this consumer with the safety asked for.

Two fixtures, with the mean taken from the card benchmark this SDK was
written against (512-byte reads at 18.5 ms mean, 150 ms worst; the TIMER's
20 ms period is 782 counts, and 100 KiB over 180 s or over 30 s is
570 B/s or 3 413 B/s):

| consumer | b (milli-B/tick) | n | P | U | start-up |
|---|---|---|---|---|---|
| 100 KiB / 180 s | 11 370 | 45 | 1 024 B (2 chunks) | 2 % | 37 ms |
| 100 KiB / 30 s | 68 325 | 7 | 1 536 B (3 chunks) | 12 % | 55 ms |

The slower consumer is served a refill every 45 ticks and pre-fetches two
chunks; the faster one needs three chunks in hand and gives the medium 12 %
of its time. Ask for 20 000 B/s on the same card and the same arithmetic
answers U = 72 %, `feasible` = 0, with a pre-fetch of 6 656 B - the honest
answer for a consumer past what the card can do.

**Batch your I/O.** The costs that matter are per *call*, not per byte:
one console write costs about 15 000 T-states however few bytes it
carries, and one card read costs about 30 000 plus the block. That is why
`putchar` and `printf` buffer a line at a time, why a `read` of a block
is worth much more than a byte loop, and why the buffered streams exist:
`tests/bench/iobench.c` (run in the filesystem image by
`tests/test_io_sim.py`'s bench replay, and on a card from the owner's
copy) measures the patterns on the machine. Measured in the simulator,
per byte: `putchar` about 1,330 T-states (one console call per 128-byte
line), a one-byte `read` about 29,000, `getc` on a buffered stream about
2,600, and 512-byte `read`s about 1,170 - a byte-at-a-time `read` pays
the call overhead per byte, while `getc` pays it once per 512.

### 6.11 The sound card (`kos/sound.h`)

```c
int sound_open(unsigned char exclusive);
int sound_close(int handle);
int sound_info(int handle, struct ksound_info *info);
int sound_write(int handle, const struct ksound_write *writes,
                unsigned int count);
int sound_reset(int handle);
void sound_out(unsigned char port, unsigned char value);
```

`/dev/sound` is the sound driver (`kos/ksound.h` has the protocol). Two
ways to use it:

- **Batched writes.** Build a sequence of `struct ksound_write`
  `{target, value}` pairs (target 0-3 a PSG, 4 the DAC; the
  `KSOUND_TARGET_*` names) and send it with `sound_write`: one call into
  the driver for the whole sequence, which is the point, since a call
  costs about 15,500 T-states. A music tick's register writes belong in
  one call. A sequence longer than one descriptor (4 KiB) is split over
  several calls; an unknown target is `-KOS_EINVAL` with nothing of that
  call written. It returns the writes performed.
- **Direct I/O.** `sound_open(1)` takes the card exclusively (a second
  exclusive open is `-KOS_EBUSY`) and resets it. `sound_info` reports the
  ports (`psg_ports[]`, `dac_port`), the PSG clock (`psg_clock_hz`) and
  the capability flags. `sound_out` writes a port the program chose at
  run time. `sound_close`, or the program's exit, resets the card.

`sound_reset` silences the card on any handle. Timing is the program's:
a write happens when it is sent, and the timer's events (6.9) pace music.
A build without the sound driver answers `-KOS_ENOENT` or `-KOS_ENODEV`
from `sound_open`.

## 7. Memory layout

```text
0100h-015Ch     the SDK's fixed low data (_KOS_SDK_DATA, 93 bytes: the
                scratch, the call, namespace and exec blocks)
015Dh           crt0 (kos_entry)
...             the program's code
0100h+image     BSS: crt0's tail copy and argv, then the program's statics
...             free memory
E000h-STACK     the stack, growing down from E000h; a delivered device
                event suspends the program 24 bytes below SP with the
                handler running below that
0100h-1FFFh     the syscall argument window: every gate-visible block is here
8000h-BFFFh     with the aperture open: a far segment or a mapped object
```

The runtime object is linked into every program and pins `_KOS_SDK_DATA`
before `_CODE`, so the gate-visible blocks stay in the argument window
however large a program's BSS grows. A program with far code or far data
keeps `8000h-BFFFh` free (the far link rule).

Buffering is part of that BSS: every program that writes to the console
carries the 128-byte output buffer (`console_proto`), and a program that
calls `fopen` carries the stream pool - four streams of 512 bytes with
their state, about 2.2 KiB. The pool is fixed, so a program's stream
memory does not depend on how many files it opens; the fifth `fopen`
returns `NULL`.

A program linked with `NO_CRT0=1` still gets the low-data area. The IPC
acceptance probe sets `LOWBASE=0x0300 CODEBASE=0x0344` to leave
`0100h-02FFh` as a plain 512-byte buffer.

## 8. What links what

`libkos` is an archive: a member is linked only when a symbol it defines is
needed.

| a program calls | members linked |
|---|---|
| anything | `kos_crt0`, `kos_runtime`, `args` |
| `console_write`, `console_read`, `console_poll`, `putchar`, `console_flush` | `console_proto`, `ipc`, `kos_ipcgate`, `nsgate` |
| `console_render` | `console_render`, plus the above |
| `console_info` | `console_info`, `ipc`, `kos_ipcgate`, `nsgate` |
| `console_puts`, `getchar`, `console_readline` | `console_io`, plus the above |
| `exit`, `getinfo`, `reboot` | `kos_gate` |
| `open`, `read`, ... | `fs`, `ipc`, `kos_ipcgate`, `nsgate` |
| `truncate` | `truncate`, `ipc`, `kos_ipcgate`, `nsgate` |
| `bind`, `call`, `unbind`, `list`, `chdir`, `getcwd` | `ipc`, `kos_ipcgate`, `nsgate` |
| `run` | `exec`, `ipc`, `kos_ipcgate`, `nsgate` |
| `timer_open`, `timer_now`, `timer_us`, `timer_wait_us` | `timer`, `ipc`, `kos_ipcgate`, `nsgate` |
| `xalloc` and friends | `xmem`, `kos_xmemgate` |
| `seginfo`, `segmap`, `farcall`, `farmap`, `blob` | `seg` |
| `xload` | `xload` |
| a `__banked` call | `kos_bcall` |
| an indirect call SDCC cannot bind at link time | `kos_sdcc_calls` |
| `diag_*` | `kos_diaggate` |
| `printf` | `printf`, `strlen`, `utoa`, `ultoa`, `itoa`, `ltoa` |
| `/` or `%` on integers | `sdcc_arith` / `sdcc_arith_signed` and their entry stubs |
| a product of two variables, or a 32-bit product | nothing: `__mullong`, which no member defines - the link fails (section 5) |
| `strcmp`, `memset`, ... | that one member |
| `fopen`, `fclose` | `stdio`, with `fs`, `timer`, `console_proto` and the IPC gates it calls into |
| `fread`, `fgetc`, `getc`, `fgets` | `fread`, plus the `stdio` group |
| `fwrite`, `fputc`, `putc`, `fputs` | `fwrite`, plus the `stdio` group (`klibc/strlen` for `fputs`) |
| `fseek`, `ftell`, `rewind`, `fflush` | `fseek`, plus the `stdio` group |
| `feof`, `ferror` | `feof` |
| `fmeasure`, `fstats` | `stream`, plus the `stdio` group |
| `stream_plan` | `stream` alone |

`libgfx` follows the same member rule; its members call into `ipc` and
the gates the same way any client does:

| a program calls | gfx members linked |
|---|---|
| `gfx_open`, `gfx_close`, `gfx_get_info` | `gfx`, `gfx_font`, `ipc`, `kos_ipcgate`, `nsgate`, `gfx_v9958_io` |
| `gfx_set_mode`, `gfx_display_enable`, `gfx_show_page`, `gfx_wait_vblank` | `gfx_mode`, `v9958_gfx`, `gfx_v9958_io` |
| `gfx_clear`, `gfx_fill_rect`, `gfx_copy_rect` | `gfx_draw`, `gfx_batch`, `v9958_gfx`, `gfx_v9958_io` |
| `gfx_rect`, `gfx_line`, `gfx_point`, `gfx_ellipse`, `gfx_fill_ellipse` | `gfx_shapes`, `gfx_batch`, `v9958_gfx`, `gfx_v9958_io` |
| `gfx_begin`, `gfx_end`, `gfx_flush`, `gfx_wait` | `gfx_batch`, `v9958_gfx`, `gfx_v9958_io` |
| `gfx_set_palette` | `gfx_palette`, `v9958_gfx`, `gfx_v9958_io` |
| `gfx_upload_begin`, `gfx_upload_write`, `gfx_upload_end` | `gfx_upload`, `gfx_v9958_io` |
| `gfx_font_set_glyph`, `gfx_font_reset_glyph`, `gfx_font_reset` | `gfx_font` |
| `gfx_text`, `gfx_text_n`, `gfx_text_width` | `gfx_text`, `gfx_batch`, `gfx_v9958_io`, `strlen` |

The backend's assembly member is pulled only through the backend C
member, so a program that calls none of the drawing entry points links
no V9958 port code.

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
`tests/test_stdio.py` compiles the console client and the stream members
with the SDK's own flags and runs the linked bytes in the host Z80, with
the console driver and the file provider stubbed by the probe: the
console's flush rules (a line at a `'\n'`, a full buffer, a flush before
every read, a failed flush's error), the stream modes and pool, reading
and writing through the buffer, and the plan arithmetic on the fixtures
of section 6.10. `tests/test_io_sim.py` runs the outcome programs in the
filesystem image from the monitor: `/system/iomix.exe` proves the console
transcript is the one an unbuffered program produces, and
`/system/iostream.exe` runs one card-file stream session and reports
every outcome.
`tests/test_sdk_package.py` checks the standalone SDK (section 11).

## 10. The demo image and the examples

`user/helloworld/Makefile` builds the examples, and

```sh
make helloworld-image
```

builds a separate tree (`build-helloworld/`) from `systems/helloworld.json`,
whose boot program is `/system/hello.exe`. Nothing else differs from
production. On a board the transcript is the demo's greeting once, the
launcher's line and then the monitor the image's acceptance policy hands
the machine to:

```text
Hello, KonaOS!
boot: /system/hello.exe exit 0
KonaOS native monitor
Z80>
```

because an acceptance image's `after_exit` is `monitor` (section 4), while
production's `relaunch` starts the shell again.

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
- **`tui_demo`**: core TUI panels, a nested window, styles, frames, a status
  row and a key loop.
- **`tui_keys`**: prints the symbolic name of every decoded TUI key.
- **`tui_dialog`**: menu, list, input, checkbox, buttons and a modal
  confirmation built on the optional widget archive.

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
  include/         kos.h, tui.h, tui_widgets.h, kos/*.h, klibc/*.h,
                   abi/kos/*.h
  lib/             libkos.lib (the runtime object included), libtui.lib,
                   libtui_128x35.lib, libtui_widgets.lib, libgfx.lib,
                   crt0.rel
  tools/           exepack.py, exemanifest.py, ihx.py (Python standard
                   library only), kimg.py (Pillow: the KIMG converter)
  mk/kos.mk        the program rule
  template/        Makefile and hello.c
  examples/        hello, fardemo, fardata, files, tui_demo, tui_keys,
                   tui_dialog
  docs/            sdk.md, tui.md, exe-format.md
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
archive's metadata, builds `template/`, `examples/` and the TUI fixture
with the tree's paths absent, runs the result through the production
image's monitor in the simulator, and checks that a copy with a changed
ABI byte is refused.

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

## 13. KRES (the resource container)

KRES is a small WAD-style named-resource container in one file
(`docs/kres-sdk-design.md` is the specification): up to 32 named byte
resources, each one contiguous run of 512-byte records, with a
crash-safe catalog commit and an explicit rebuild compaction. Sealed
containers are built once and only read. The format is v1.

The portable core lives in `user/sdk/kres/src/` with the public header
`kos/kres.h`. The core reaches storage only through a small adapter,
`struct kres_io` (open/close/size/read/write/seek/sync/truncate/rename/
unlink); the host edition ships a stdio adapter, the KonaOS edition the
SDK file API. A program that does not link libkres pays nothing for it.

The API in one paragraph:

- `kres_install(io, cache, ctx)` installs the adapter, the catalog
  cache and the per-process context once; `kres_install_mut` also
  installs the read path's mutation-side hooks (create-on-missing and
  close-flush).
- `kres_open(c, path, flags)` binds a container (KRES_OPEN_READ/WRITE/
  CREATE), `kres_close` flushes and closes, `kres_flush` commits.
- `kres_get`, `kres_create`, `kres_read`, `kres_write`, `kres_seek`,
  `kres_resize`, `kres_reserve`, `kres_read_records`,
  `kres_write_records` work one resource; `kres_rclose` closes a
  descriptor. Record operations do not touch the cursor; a write past
  the logical EOF is refused, a write at it extends.
- `kres_stat`, `kres_delete`, `kres_rename`, `kres_iter_begin`,
  `kres_iter_next` walk the catalog.
- `kres_compact(path, flags)` rebuilds in a temporary file and swaps
  names: by default preserving reservations, KRES_TIGHT dropping slack,
  KRES_SEAL producing a sealed container.
- `kres_extract` copies one resource's bytes into a new file.
- `kres_verify` is the validation pass: it fails closed on a malformed
  file and never follows an unchecked offset.
- `kres_build_begin/add/write/end` build a sealed container; an
  interrupted build is an invalid file (`kres_build_abort` closes it).

Errors are negated KOS codes; KRES_E_FULL (catalog full), KRES_E_RECLIM
(the 65,535-record limit), KRES_E_BAD (not a KRES v1 file) and
KRES_E_GROW are the format's own.

The host tool `build/host/kres` (`make kres-host`) implements create,
list, show, import, extract, unpack, pack, verify, compact and seal.
Names are CP850 bytes converted at the host boundary only; payloads are
byte for byte. Export refuses unsafe paths and case-insensitive
collisions.

The archive member rule applies: the read path, the mutation and flush
machinery, the builder and the compactor are separate members, so a
program that only reads sealed containers links none of the
catalog-commit or compaction code.

The KonaOS edition ships as `libkres.lib`, linked with `LIBS=kres`
(like `LIBS=gfx`). Its members:

| member | holds |
|---|---|
| `kres_util.rel` | the install, header parse, entry codecs, shared buffer |
| `kres_read.rel` | open/close/get/read/seek, the read path |
| `kres_adapter.rel` | the KonaOS adapter's read side |
| `kres_cache_xmem.rel` | the 2 KiB XMEM catalog cache |
| `kres_mut.rel` | mutation, flush and the growth machinery |
| `kres_adapter_mut.rel` | the adapter's write/truncate/rename/unlink |
| `kres_build.rel` | the sealed builder |
| `kres_compact.rel` | rebuild compaction |
| `kres_extract.rel` | one-resource export |
| `kres_verify.rel` | the validation pass |

A sealed-container reader links only the first four members and calls
`kres_install(&kres_io_konaos_read, &kres_cache_xmem, &ctx)` (from
`kos/kres_konaos.h`); a mutating program calls
`kres_install_mut(&kres_io_konaos, &kres_cache_xmem, &ctx)`. The
adapter keeps two file slots keyed by container and the card serves at
most two open files system-wide, so a tool flushes and **closes its
container before `run()`ing the next tool** (design §9.1); compaction
holds both slots. The cache is one 2 KiB XMEM object per open
container, allocated at open and freed at close. A write beyond the
physical end extends the file with zero records; compaction's
temporary and recovery files spell with 8.3 names in the container's
directory. `krestest` (tests/kres) and `tests/test_kres_native.py` are
the native acceptance program and replays.
