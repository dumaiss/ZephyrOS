# KonaOS Graphics SDK Architecture — GFX v1

**Status:** Accepted design  
**Target:** KonaOS / Zephyr-80 / V9958  
**Scope:** first native graphics SDK sufficient for a picture viewer and simple bitmap text  
**Library:** `libgfx`

---

## 1. Purpose

`libgfx` is the user-space graphics SDK for native KonaOS applications.

It provides a small graphics API above the machine's video device while keeping high-level graphics functionality out of the resident operating system.

The first version has two concrete goals:

1. support a picture viewer that can display a host-prepared image and return cleanly to the shell;
2. support simple fixed-width text suitable for labels, status information and the first graphical applications.

It is deliberately **not** a GUI toolkit. Windows, controls, menus, layout, widgets, proportional fonts and desktop policy belong to a later `libui`.

The layering is:

```text
application
    |
    | gfx_*()
    v
libgfx
    |
    | direct hardware I/O while VIDEO is exclusively owned
    v
video hardware
```

KonaOS remains responsible for ownership and recovery:

```text
application
    |
    | exclusive bind / release
    v
/dev/video
    |
    v
VIDEO driver
```

The VIDEO driver is not in the normal drawing path.

---

## 2. Design principles

### 2.1 Graphics policy belongs in the SDK

The resident VIDEO driver owns:

- device discovery/publication;
- exclusive ownership;
- hardware initialization needed by the console;
- recovery when exclusive ownership ends;
- VDP interrupt ownership where required;
- enough hardware description to allow an exclusive SDK client to operate the device.

`libgfx` owns:

- graphics-mode setup while it owns the VDP;
- accelerated drawing operations;
- batching;
- pixel upload;
- palettes;
- VRAM use for SDK resources;
- fonts;
- glyph expansion and caching;
- text rendering.

The application owns:

- its visual design;
- images and other assets;
- redraw policy;
- higher-level UI state.

No font renderer, image decoder, widget toolkit or graphics command queue is added to the nucleus or resident driver.

### 2.2 Exclusive ownership enables the fast path

`gfx_open()` binds `/dev/video` with `KOS_BIND_EXCLUSIVE`.

While the handle is held:

- console output to the VDP is suppressed;
- `libgfx` may operate the VDP directly;
- graphics operations do not cross IPC;
- the application is responsible for the hardware state it establishes.

When the handle is released, the V9958 driver restores its normal console configuration.

Therefore:

```text
gfx_open()
    -> one bind/acquire
    -> one hardware-info call

drawing
    -> direct I/O only

gfx_close()
    -> unbind/release
    -> driver restores console
```

Process teardown also releases the handle, so an application that exits without calling `gfx_close()` still returns the display to a defined OS state.

### 2.3 Operations describe intent, not Yamaha command names

Public API names describe graphics operations:

```text
fill
copy/blit
upload
text
palette
```

They do not expose names such as HMMM, HMMV or LMMC.

The V9958 backend maps these semantic operations to the best available VDP command.

For GFX v1, the normal path intentionally favors the V9958's high-speed commands.

### 2.4 No per-pixel or per-glyph IPC

Graphics calls execute in application space.

A sequence such as:

```c
gfx_begin();

gfx_fill_rect(...);
gfx_text(...);
gfx_fill_rect(...);

gfx_end();
```

is accumulated by `libgfx` and executed through a tight backend command loop.

A string does not cause one OS call per character.

### 2.5 Host-prepared assets are preferred

The Z80 does not decode PNG, JPEG or another compressed desktop format merely to prove graphics operation.

Host tooling converts source images into a simple VDP-ready Kona image representation.

The target viewer performs only:

```text
open
read header
set mode
stream pixels
show display
wait for key
exit
```

---

# 3. VIDEO ABI support

## 3.1 Existing VIDEO interface

The existing VIDEO protocol remains responsible for normal console/video service and lifecycle.

Its existing `KVIDEO_OP_INFO` remains the console-oriented geometry/capability operation and is not changed into a graphics-hardware query.

## 3.2 Direct graphics information

GFX requires one new VIDEO operation, `KVIDEO_OP_DIRECT_INFO` (6,
`abi/kos/protocol.h`). Its reply record and constants live in
`abi/kos/kvideo.h`, which the SDK ships, as `kos/ksound.h` does for the
sound card.

It is served only to the exclusive holder of `/dev/video`; any other
caller gets `-KOS_EBUSY`.

A proposed reply is:

```c
struct kvideo_direct_info {
    uint8_t  version;
    uint8_t  backend;
    uint16_t flags;

    uint16_t vram_kib;

    uint8_t  data_port;
    uint8_t  command_port;
    uint8_t  palette_port;
    uint8_t  indirect_port;
    uint8_t  config_port;

    uint8_t  config_initial;
    uint8_t  reserved[7];
};
```

The record fits comfortably inside one IPC reply.

Initial backend identifiers include:

```c
#define KVIDEO_BACKEND_V9958  1
```

Initial capability flags include only properties needed by the backend, for example:

```c
KVIDEO_DIRECT_IO
KVIDEO_HW_WAIT
KVIDEO_PALETTE
KVIDEO_YJK
```

Port numbers are returned individually. `libgfx` does not assume that future hardware places registers at the same addresses or contiguously.

Applications do not normally inspect this structure. It is consumed internally by the `libgfx` backend.

`config_initial` is the configuration-port value the console uses (its
interrupt route in D0, the porch bit in D1); libgfx writes the
configuration port only to keep that value.

---

# 4. SDK organization

The SDK is a separate static library:

```text
libkos.lib
libtui.lib
libgfx.lib
```

An application that does not use graphics pays no code or data cost for `libgfx`.

Internally:

```text
libgfx
 |
 +-- gfx.c                 public API and state
 +-- gfx_batch.c           command queue
 +-- gfx_text.c            font and text support
 +-- gfx_image.c           optional KIMG helpers
 |
 `-- backends/
      `-- v9958/
           +-- mode setup
           +-- command execution
           +-- streaming upload
           +-- palette
           `-- low-level port I/O
```

The application's source includes only:

```c
#include <kos/gfx.h>
```

Backend-specific headers are private to the SDK.

---

# 5. Process-local graphics state

GFX v1 has one graphics context per process.

There is no heap allocation and no need for multiple simultaneously opened graphics devices.

Conceptually:

```c
struct gfx_state {
    int video_handle;

    uint8_t backend;
    uint16_t caps;

    enum gfx_mode mode;
    uint16_t width;
    uint16_t height;

    /* private backend state */
    ...

    /* command batch */
    ...

    /* font/cache state */
    ...
};
```

This state is private to `libgfx`.

The public API does not require the application to pass a context pointer to every operation.

---

# 6. Graphics modes

GFX exposes semantic mode names rather than V9958 mode numbers.

Initial modes:

```c
enum gfx_mode {
    GFX_MODE_512x212_16,
    GFX_MODE_256x212_256,
    GFX_MODE_256x212_YJK
};
```

On Zephyr-80 these map to:

```text
GFX_MODE_512x212_16
    -> V9958 GRAPHIC 6
    -> 4 bpp
    -> programmable 16-color palette

GFX_MODE_256x212_256
    -> V9958 GRAPHIC 7 / 256-color form
    -> 8 bpp

GFX_MODE_256x212_YJK
    -> V9958 GRAPHIC 7 + YJK
    -> host-prepared YJK pixels
```

The intended usage is:

```text
512×212×16
    normal GUI applications
    tracker
    editor
    future GEOS-like desktop

256×212×256 / YJK
    picture viewer
    artwork/media applications
```

GFX v1 does not expose interlaced modes: every mode is 212 lines,
non-interlaced (R#9 = `80h`). The release restores the console's own
display timing.

**Coordinates and pages.** Both GRAPHIC 6 and GRAPHIC 7 have two 256-line
VRAM pages. y runs 0-511: lines 0-211 are the visible screen (page 0) and
lines 256-511 the off-screen page, which belongs to the application while
libgfx holds the display (section 15).

---

# 7. Core lifecycle API

The minimum lifecycle surface is:

```c
int  gfx_open(void);
void gfx_close(void);

int gfx_set_mode(enum gfx_mode mode);

int gfx_display_enable(uint8_t enabled);

int gfx_get_info(struct gfx_info *info);
```

`gfx_open()`:

1. exclusively binds `/dev/video`;
2. verifies the returned object type is VIDEO;
3. requests `KVIDEO_OP_DIRECT_INFO`;
4. selects the appropriate backend;
5. initializes process-local graphics state.

It does not need to enter a display mode immediately.

`gfx_set_mode()` establishes a complete known graphics state from any VDP state.

The SDK does not depend on console register state.

`gfx_close()` flushes pending work and releases the VIDEO handle. The VIDEO driver's release path restores the console.

---

# 8. Basic drawing operations

GFX v1 provides a deliberately small primitive set:

```c
typedef uint8_t gfx_color_t;

int gfx_clear(gfx_color_t color);

int gfx_fill_rect(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    gfx_color_t color);

int gfx_copy_rect(
    uint16_t src_x,
    uint16_t src_y,
    uint16_t dst_x,
    uint16_t dst_y,
    uint16_t width,
    uint16_t height);
```

The initial semantics are **COPY only**.

No public raster-operation parameter is necessary in GFX v1.

On the V9958:

```text
gfx_clear/fill_rect
    -> HMMV

gfx_copy_rect
    -> HMMM, with direction chosen automatically
```

`gfx_copy_rect()` detects overlap and selects a safe copy direction. Applications do not specify VDP direction bits.

**Alignment in GRAPHIC 6.** Two pixels share a byte, and the high-speed
commands and direct writes move whole bytes, so in
`GFX_MODE_512x212_16` every x and width passed to a drawing, upload or
text call must be even; an odd one is `-KOS_EINVAL`. The 256-pixel modes
take any x.

Any operation may use the off-screen page (y 256-511) as its source or
destination, so a program keeps its tiles, sprites and icons in VRAM and
places them with `gfx_copy_rect()`.

Logical and transparent operations may be added later when an application demonstrates the need for them.

---

# 9. Command batching

## 9.1 Purpose

Even though drawing is direct and does not cross IPC, repeatedly setting up and executing VDP commands from independent high-level C calls is unnecessarily expensive.

GFX therefore provides explicit batching:

```c
void gfx_begin(void);
int  gfx_end(void);
int  gfx_flush(void);
```

Inside a batch, drawing operations append compact records to a process-local command buffer.

A record is fixed-size and backend-independent, approximately:

```c
struct gfx_cmd {
    uint8_t opcode;
    uint8_t flags;

    uint16_t sx;
    uint16_t sy;

    uint16_t dx;
    uint16_t dy;

    uint16_t width;
    uint16_t height;

    uint8_t color;
    uint8_t reserved;
};
```

A 16-byte command permits a useful fixed queue with little application RAM.

For example:

```text
32 commands = 512 bytes
64 commands = 1 KiB
```

The exact default is an SDK build choice.

When the queue becomes full, `libgfx` automatically flushes it.

## 9.2 Execution

The V9958 backend walks the batch in order:

```text
FILL -> wait as required -> program command registers -> HMMV
COPY -> wait as required -> program command registers -> HMMM
...
```

GFX v1 does not reorder commands.

Ordering is part of the API contract.

## 9.3 Calls outside a batch

A primitive called outside `gfx_begin()` / `gfx_end()` executes immediately.

A high-level operation such as `gfx_text()` may still construct a temporary internal batch so a string is never rendered as a sequence of unrelated high-level operations.

---

# 10. Streaming pixel upload

Images are too large to place in a graphics command batch.

GFX therefore has a separate streaming path:

```c
int gfx_upload_begin(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height);

int gfx_upload_write(
    const void *data,
    uint16_t length);

int gfx_upload_end(void);
```

Typical viewer code is:

```c
gfx_upload_begin(0, 0, width, height);

while ((n = read(file, buffer, sizeof(buffer))) > 0)
    gfx_upload_write(buffer, n);

gfx_upload_end();
```

The upload is a run of direct VRAM writes through the data port: libgfx
keeps the position inside the rectangle across calls, setting one VRAM
address per row (one for the whole transfer when the rectangle spans the
full width). Chunks may have any length, so a program never needs the
whole image in RAM: it streams from a file, from extended memory (through
`xread` into a small buffer) or from data it generates.

A streaming upload:

- flushes any pending drawing batch before it begins;
- cannot be nested;
- must receive exactly the number of bytes implied by its geometry and current mode;
- may target the off-screen page, so assets are loaded once and copied
  to the screen afterwards.

A 4-KiB application file buffer is a sensible default but is not part of the API.

The pixel packing is determined by the current mode:

```text
512×212×16
    two 4-bit pixels per byte

256×212×256
    one pixel per byte

256×212 YJK
    one host-encoded V9958 YJK byte per pixel
```

---

# 11. Palette API

Palette modes provide:

```c
struct gfx_rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

int gfx_set_palette(
    uint8_t first,
    uint8_t count,
    const struct gfx_rgb *colors);
```

`r`, `g` and `b` use the portable SDK representation; the backend converts them to the V9958 palette representation.

For `GFX_MODE_512x212_16`, at most 16 entries exist.

A mode without a programmable application palette returns `-KOS_ENOTSUP`.

Palette programming is direct I/O and does not require a VIDEO protocol call after ownership has been acquired.

---

# 12. Text model

## 12.1 Text belongs to libgfx

Text is an application-facing graphics primitive even though it is implemented from bitmap glyphs and blits.

The application does not need to:

- upload a font;
- reserve VRAM for glyphs;
- calculate glyph coordinates;
- know which VDP command renders a glyph.

A graphical program can immediately call:

```c
gfx_text(8, 8, "KonaOS", &style);
```

after entering a supported graphics mode.

## 12.2 Default font

GFX v1 supplies one built-in font:

```text
6 × 8 pixels
256 glyphs
8 bytes per glyph
CP850-compatible character layout
```

The canonical bitmap therefore occupies:

```text
256 × 8 = 2048 bytes
```

The font belongs to `libgfx`, not the VIDEO driver.

The source font asset is shared at build time with the console font (the
same pinned `font_cp850_6x8.inc`); there is no runtime dependency between
them. `gfx_open()` copies the built-in font into a 2,048-byte RAM font
that the glyph calls below change.

## 12.3 Mutable characters

Applications may redefine glyphs, in the same spirit as programmable character sets on machines such as the C64.

The API is:

```c
int gfx_font_set_glyph(
    uint8_t code,
    const uint8_t rows[8]);

int gfx_font_reset_glyph(uint8_t code);

void gfx_font_reset(void);
```

Only the low six bits of each row are significant in the default 6×8 font.

This allows applications to reserve character codes for application-specific symbols:

```text
80h   play
81h   stop
82h   pause
83h   record
84h   arrow
...
```

No separate icon subsystem is required for the first applications.

---

# 13. Text styles and rendering

GFX v1 text is intentionally **opaque**.

```c
struct gfx_text_style {
    gfx_color_t fg;
    gfx_color_t bg;
};
```

The basic API is:

```c
int gfx_text(
    uint16_t x,
    uint16_t y,
    const char *text,
    const struct gfx_text_style *style);

int gfx_text_n(
    uint16_t x,
    uint16_t y,
    const char *text,
    uint16_t length,
    const struct gfx_text_style *style);

uint16_t gfx_text_width(const char *text);
```

For the default font:

```text
width  = character_count × 6
height = 8
```

`gfx_text()` is single-line and byte-oriented.

It does not interpret:

- newlines;
- tabs;
- ANSI escapes;
- UTF-8;
- markup.

Those are higher-level facilities.

---

# 14. Text rendering path

Text is drawn by the CPU, straight into VRAM, in two stages:

```text
glyph rows (RAM font)
    |
    v
expansion   font row bits -> pixels in the style's colours
    |
    v
pixel sink  places the pixels on the screen
```

**Expansion** turns a glyph's 8 rows into pixels for the requested
foreground/background pair: in GRAPHIC 6 three bytes per row (two pixels
each), through three 256-entry tables built for the pair and kept for the
most recent pairs; in GRAPHIC 7 six bytes per row, one per pixel.

**The opaque sink** (GFX v1) writes those bytes through the data port, one
VRAM address per glyph row, with the hardware WAIT pacing the writes. A
string is one run: no command engine, no per-glyph setup beyond the row
addresses.

**The transparent sink** (a later addition) feeds the same expanded pixels
to the V9958's logical CPU-to-VRAM transfer (LMMC) with the transparency
flag set, so background pixels (colour 0) are skipped by the VDP. Only the
sink changes; the font, the expansion and the text calls stay as they are.
Its R#44 stream is written with R#17's auto-increment off.

There is no glyph atlas and no VRAM cache: redefining a glyph changes its
8 bytes in the RAM font and the next draw uses them.

Measured on the board (6x8 glyph): direct writes about 1,620 T-states, an
HMMM copy from a VRAM atlas about 3,380.

---

# 15. VRAM ownership

While `libgfx` holds the display, the application owns the graphics-mode
VRAM through coordinates: the visible page (y 0-211) and the off-screen
page (y 256-511). libgfx keeps no private VRAM resources in GFX v1, so it
needs no allocator, and the application addresses its off-screen assets
by coordinate.

There is no kernel VRAM allocator. There is no cross-process VRAM
ownership problem, because the machine is single-tasking and VIDEO is held
exclusively. The release re-runs the console bring-up, which rewrites
every VRAM table the console uses.

---

# 16. Simple image format

The first picture viewer uses a host-prepared `KIMG` format.

It exists to avoid putting PNG/JPEG decoding on the Z80.

A v1 header is:

```text
offset size field

00     4   magic = "KIMG"
04     1   version = 1
05     1   mode
06     1   flags = 0
07     1   palette_count
08     2   width
0A     2   height
0C     4   payload_size
```

All multibyte fields are little-endian.

The header is followed by:

```text
palette_count × RGB triples
pixel payload
```

Mode uses the same semantic values as `enum gfx_mode`.

Initial payload formats are:

```text
512×212×16
    packed 4 bpp, two pixels per byte

256×212×256
    one native 8-bit pixel per byte

256×212 YJK
    one host-generated YJK byte per pixel
```

The host converter is `tools/kimg.py`, shipped in the SDK's `tools/`. It
requires Pillow and accepts the formats Pillow reads (PNG, JPEG, ...). It
performs:

- scaling;
- cropping;
- color reduction;
- palette generation;
- RGB-to-YJK conversion;
- pixel packing.

The target does none of these.

The target format does not depend on the source format. Test fixtures are
committed pre-converted, so neither the build nor the tests need Pillow.

---

# 17. Picture viewer

The first graphical application is `pview.exe`, a card application built
with the standalone SDK like any other user program
(`UserSoftware/PView/`).

Usage:

```text
pview picture.kimg
```

Its complete lifecycle is:

```text
open KIMG
    |
read/validate header
    |
gfx_open()
    |
gfx_set_mode(image.mode)
    |
disable display
    |
set palette if present
    |
gfx_upload_begin()
    |
read file in useful blocks
    |
gfx_upload_write()
    |
gfx_upload_end()
    |
enable display
    |
wait for key
    |
gfx_close()
    |
return to shell
```

For a full-screen image:

```text
G6: 512 × 212 × 4 bpp = 54,272 bytes
G7: 256 × 212 × 8 bpp = 54,272 bytes
```

Thus the high-color viewer does not inherently require more framebuffer I/O than the high-resolution 16-color mode.

The first viewer SHOULD support:

```text
GFX_MODE_256x212_256
GFX_MODE_256x212_YJK
```

G6 image support is useful but is not required to prove the first viewer milestone.

The viewer uses the normal Kona input path to wait for a key; it requires no special graphics-input mechanism.

---

# 18. Error and recovery rules

Every public function returns 0 or a negative Kona error unless it naturally returns a value.

`gfx_open()` is atomic from the application's perspective:

```text
failure before exclusive acquire
    -> no graphics state exists

failure after acquire
    -> release VIDEO before returning the failure
```

`gfx_close()` is safe after any successful `gfx_open()`.

A failed graphics application must never require reboot merely to recover the console.

Because process exit releases handles, the OS remains the final recovery mechanism even when application cleanup is skipped.

---

# 19. Performance rules

The first implementation follows these rules:

1. **No primitive drawing IPC.**
2. **No per-glyph IPC.**
3. **No per-pixel syscall or IPC path.**
4. Repeated drawing operations use a local batch.
5. Large images use one continuous streaming upload, fed from file-sized chunks.
6. Opaque text uses cached precolored glyphs and high-speed copies.
7. Logical VDP commands are not used unless their semantics are actually required.
8. The SDK does not clear, copy or rebuild resources unnecessarily.
9. Mode changes establish complete known state rather than depending on what the console previously programmed.

---

# 20. Explicit non-goals for GFX v1

GFX v1 does not provide:

```text
windows
widgets
menus
buttons
mouse handling
proportional fonts
UTF-8
font files
anti-aliased text
transparent text
alpha blending
general raster operations
application-visible VRAM allocation
arbitrary surfaces
image scaling on the Z80
PNG/JPEG decoding on the Z80
interlaced modes
double buffering
compositing
a retained scene graph
```

None of these are requirements for the viewer or basic text.

They are added only when a real application requires them.

---

# 21. Future extension boundary

The intended future stack is:

```text
applications
    |
   libui
    |
   libgfx
    |
direct graphics backend
    |
exclusive /dev/video ownership
```

`libui` may eventually provide:

```text
windows
controls
menus
dialogs
text fields
scrollbars
layout
focus
mouse interaction
```

Those features do not move into `libgfx` or the VIDEO driver.

The future tracker should consume `libgfx` directly first. Reusable UI behavior discovered while implementing real graphical applications may later be extracted into `libui`.

---

# 22. Initial implementation packages

## GFX-1 — exclusive graphics foundation

Implement:

```text
KVIDEO_OP_DIRECT_INFO
gfx_open
gfx_close
V9958 direct-I/O backend
gfx_set_mode
gfx_display_enable
```

Acceptance:

```text
shell
 -> gfx test enters G6
 -> clears screen
 -> waits
 -> exits
 -> normal console is completely restored
```

No graphics draw operation uses VIDEO IPC after initialization.

## GFX-2 — streaming image upload and the picture viewer

Implement:

```text
gfx_upload_begin
gfx_upload_write
gfx_upload_end
gfx_set_palette
KIMG v1
```

Build a host-prepared image fixture.

Acceptance:

```text
pview test.kimg
 -> enters G7
 -> displays the complete image
 -> keypress
 -> returns to a correct shell
```

Add YJK as the high-color acceptance case.

## GFX-3 — accelerated primitives and batching

Implement:

```text
gfx_begin
gfx_end
gfx_flush
gfx_clear
gfx_fill_rect
gfx_copy_rect
```

V9958 mapping:

```text
FILL -> HMMV
COPY -> HMMM
```

Acceptance proves command ordering, overlap-safe copies and automatic batch flushing.

## GFX-4 — simple text

Implement:

```text
built-in 6×8 font (RAM copy)
gfx_text
gfx_text_n
gfx_text_width
gfx_font_set_glyph
gfx_font_reset_glyph
gfx_font_reset
expansion and the opaque pixel sink
```

Text support is initially required in:

```text
GFX_MODE_512x212_16
GFX_MODE_256x212_256
```

Pure YJK text is not required for GFX v1.

Acceptance:

```text
draw several text strings
draw at least two foreground/background pairs
redefine one glyph
draw it
restore it
draw it again
exit and recover the console
```

The renderer writes glyphs directly; the expansion and the sink are separate
so that the transparent sink can be added without changing them.

---

# 23. GFX v1 completion criterion

GFX v1 is complete when both of these programs work on physical hardware:

```text
pview
-----
shell -> high-color image -> key -> shell


gfxdemo
-------
shell
 -> G6
 -> fills
 -> copied rectangles
 -> normal text
 -> multiple text colors
 -> application-defined glyph
 -> shell
```

Both must leave the display and console in their normal KonaOS state when they exit.

At that point the platform has enough graphics infrastructure to begin the first real graphical application without adding further resident OS machinery.