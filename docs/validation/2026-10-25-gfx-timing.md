# GFX timing harness: the T-state clock (package 95c01)

Package 95c01 is a correction with no libgfx behaviour change: the
timing harness was reporting "T-states" that were actually CPU
instructions.  The V9958 peer's clock now runs on T-states, the
command-duration model runs at the board's real clock ratio, every
libgfx figure was re-measured, each one was profiled per function, and
the timing pins were re-anchored at the measured figures.

## The clock ratio

The board runs the Z80 at 10 MHz (`docs/architecture.md`; the same
10 MHz the console/TUI cost paragraphs in `docs/sdk.md` use) and the
V9958 on its 21.477272 MHz crystal (the MSX V9958 clock; the
Zephyr-80 schematic's X2).  `tests/z80sim.py` names the constants once:

- `CPU_HZ = 10000000`
- `VDP_HZ = 21477272`

so one VDP tick is `VDP_HZ / CPU_HZ` = 2.1477272 CPU T-states.  The
peer's `now` is `Z80.tstates` at every VDP I/O in both cores (the C
core calls the same `io_in`/`io_out`), and every timestamp the peer
records - `reg_writes`, `status_reads`, `addr_writes`, the command
records, `busy_until`, the frame clock - is in T-states.  The default
`command_busy` is 4,000 T-states (about 400 instructions in the CE
poll loop), matching the previous 400-instruction window.

## The duration model

`V9958Peer._command_duration` returns T-states.  The per-unit VDP
ticks come from openMSX's `VDPCmdEngine` (which reproduces the chip;
display on, sprites off): 48 VDP ticks per byte for HMMV, 88 for
HMMM, 96 per dot for LMMV, 120 for LMMM, 84 per dot plus 24 for the
first pixel for LINE, 112 for one PSET, and 40 ticks of issue
overhead.  Each total is converted at `CPU_HZ // VDP_HZ`.  The
display's access-slot contention is omitted, so the model is a lower
bound, about 20% under the chip - the same bound the package-95
docstring cites.

## The figures

The timing pins are the R#16 = 15 register writes in `gfxtest timing`;
each figure is the T-state span of its marker pair.  The first five
pairs run with the duration model off (the historical conditions);
the frame clock and the duration model come on from the 12th marker,
so the package-96 workloads' engine waits are the modeled times.

| figure | T-states | target | pin (measured + 10%) |
|---|---|---|---|
| one aligned `gfx_fill_rect`, queue + flush | 3,803 | 1,200 | 4,200 |
| `gfx_line`, any octant | 2,793 | 1,500 | 3,100 |
| filled circle r=50 | 1,401,366 | 150,000 | 1,541,600 |
| outline circle r=50 | 2,039,101 | 250,000 | 2,243,100 |
| 128-segment polyline, 512x48 | 725,495 | report | 798,100 |
| 128-glyph 4x6 G6 row, fresh colour pair | 286,086 | 100,000 | 314,700 |
| the same row, cached | 234,197 | 100,000 | 257,700 |
| 512x6 stripe fill, queue only | 3,803 | 1,200 | 4,200 |
| `gfx_show_page`, including its vblank wait | 197,315 | report | - |
| one `gfx_wait_vblank` alone | 166,299 | report | - |
| the partial present's CPU | 5,936 | report | 6,600 |

The page switch itself is ~31,000 T-states (the show-page figure
minus the vblank figure).  The partial present copies a 128x40 G6
band from the hidden page to the shown one; the duration model keeps
the engine busy for 104,910 T-states (88 VDP ticks x 2,560 bytes plus
40), so the CPU overlaps 98,974 of them after `gfx_copy_rect`
returns - the test pins the copy block, the VDP time and the overlap.

The text and atlas figures (TestGfxAtlas, one pair per run):

| figure | T-states | target |
|---|---|---|
| 128-glyph 4x6 G6 row, fresh colour pair | 234,163 | 100,000 |
| opaque 6x8 G6 string, cached pair (8 glyphs) | 55,458 | 8,000 |
| opaque 4x6 G7 string (8 glyphs) | 33,360 | 6,000 |
| transparent G6, 4 glyphs (CPU, plus the LMMM) | 40,591 | 4,000 |
| transparent G7, 4 glyphs (CPU, plus the LMMM) | 40,266 | 4,000 |
| 95-glyph atlas build, G6 | 2,333,664 | report |
| stale glyph redrawn into every atlas | 211,503 | report |

### The supervisor's spot-check

The supervisor re-measured the same markers on an earlier build; the
new binary reproduces them where the code layout is unchanged:

- fill: 3,803 T-states, exactly the supervisor's figure;
- the line and the filled/outline circles shifted between builds
  (line 4,692 -> 2,793; filled 1,430,723 -> 1,401,366; outline
  2,097,885 -> 2,039,101) because gfxtest's link layout moved
  functions across the bank-crossing call boundary, changing the
  thunk cost.  The pins follow the measured figures of the current
  binary; the atlas and text figures reproduce (55,458, 2,333,664,
  211,503 exactly).

## The profiles

`tests/gfxprof.py` (modelled on `work/88-konaedit-view/keprof.py`)
runs the same twelve workloads once, attributes every step's
T-states to the function at PC (SDCC listings for the statics, the
map for the globals and the assembly members), and prints each
figure's functions until 90% of the total.  `app:` names are the
program's bank; `osN:` names are approximate (bank N, kernel map).
Assembly members keep their `gfx_v9958_io_` names.

```
== glyph: 407,699 T-states
     365,755  89.7%      1 app:_gfx_text_build_pages   (C)
      15,174   3.7%      1 app:_gfx_text_run           (C)
== fill: 3,803 T-states
       1,707  44.9%      1 app:_gfx_fill_rect          (C)
       1,260  33.1%      1 app:_gfx_batch_submit       (C)
         378   9.9%      1 app:_gfx_rect_ready         (C)
         156   4.1%      0 app:_gfxtest_timing         (C)
== line: 2,793 T-states
       1,260  45.1%      1 app:_gfx_batch_submit       (C)
       1,092  39.1%      1 app:_gfx_line               (C)
         156   5.6%      0 app:_gfxtest_timing         (C)
          76   2.7%      1 app:_gfx_shape_op           (C)
== filled: 1,401,304 T-states
     284,162  20.3%     96 app:_gfx_v9958_io_wait_command   (asm)
     262,708  18.7%    380 app:__mulint                (C)
     150,894  10.8%      1 app:_gfx_ellipse_run        (C)
     127,501   9.1%    101 app:_gfx_batch_submit       (C)
     103,164   7.4%     56 app:_gfx_span_visit         (C)
      93,491   6.7%     92 app:_gfx_mul32              (C)
      85,661   6.1%      1 app:_gfx_ellipse_walk       (C)
      64,679   4.6%      1 app:_memset                 (C)
      49,920   3.6%      0 app:_gfx_v9958_io_start_command  (asm)
      46,460   3.3%    101 app:_gfx_shape_submit_span  (C)
== outline: 2,039,101 T-states
     571,241  28.0%    192 app:_gfx_v9958_io_wait_command   (asm)
     262,670  12.9%    380 app:__mulint                (C)
     255,021  12.5%    202 app:_gfx_batch_submit       (C)
     173,638   8.5%      1 app:_gfx_ellipse_run        (C)
     103,202   5.1%     56 app:_gfx_span_visit         (C)
      99,840   4.9%      0 app:_gfx_v9958_io_start_command  (asm)
      93,472   4.6%     92 app:_gfx_mul32              (C)
      92,958   4.6%    202 app:_gfx_shape_submit_span  (C)
      85,680   4.2%      1 app:_gfx_ellipse_walk       (C)
      76,627   3.8%    192 app:_gfx_v9958_io_run_lmmv  (asm)
      75,850   3.7%      6 app:_v9958_run_batch        (C)
== polyline: 725,526 T-states
     160,411  22.1%    127 app:_gfx_batch_submit       (C)
     139,202  19.2%    128 app:_gfx_line               (C)
      96,617  13.3%    126 app:_gfx_v9958_io_run_line  (asm)
      89,314  12.3%    128 app:_gfx_v9958_io_wait_command   (asm)
      69,475   9.6%      0 app:_gfxtest_timing         (C)
      66,560   9.2%      0 app:_gfx_v9958_io_start_command  (asm)
      48,160   6.6%      4 app:_v9958_run_batch        (C)
== row-new: 286,086 T-states
     205,497  71.8%      6 app:_gfx_v9958_io_text_row  (asm)
      34,221  12.0%      1 app:_memcpy                 (C)
      11,404   4.0%      1 app:_gfx_font4_build_pair   (C)
      11,189   3.9%      1 app:_gfx_text_run           (C)
== row-cached: 234,197 T-states
     205,497  87.7%      6 app:_gfx_v9958_io_text_row  (asm)
      11,208   4.8%      1 app:_gfx_text_run           (C)
== stripe: 3,803 T-states
       1,707  44.9%      1 app:_gfx_fill_rect          (C)
       1,260  33.1%      1 app:_gfx_batch_submit       (C)
         378   9.9%      1 app:_gfx_rect_ready         (C)
         156   4.1%      0 app:_gfxtest_timing         (C)
== page-switch: 197,284 T-states
     156,553  79.4%      1 app:_gfx_v9958_io_wait_vblank    (asm)
      32,003  16.2%      2 app:_gfx_v9958_io_wait_command   (asm)
== vblank: 166,299 T-states
     159,951  96.2%      1 app:_gfx_v9958_io_wait_vblank    (asm)
== present: 5,936 T-states
       1,517  25.6%      1 app:_gfx_copy_rect          (C)
       1,279  21.5%      1 app:_gfx_batch_submit       (C)
         756  12.7%      2 app:_gfx_rect_ready         (C)
         477   8.0%      0 os0:irq_common_entry        (kernel)
         420   7.1%      0 os7:_irq_dispatch           (kernel)
         343   5.8%      0 os7:_api_console_getc       (kernel)
         210   3.5%      2 app:_gfx_rect_check_y       (C)
```

The figures separate cleanly: the ellipse figures spend their time
in the engine wait, the multiplications and the span queue; the text
figures are the assembly text-row emitter; the page figures are the
vblank wait; the present is the copy queue with the kernel's key
scan at the margin.  The duration model's VDP time is the partial
present's 104,910 T-states above; the other VDP-bound figures (the
copies, the transparent glyphs, the fills) overlap their engine
waits the same way the model describes.

## Along the way

Two package-95 test binaries were stale, and fixing the harness
exposed them:

- `UserSoftware/GfxDemo/src/gfxdemo.c` still called the package-94
  `gfx_font_set_glyph`/`gfx_font_reset_glyph` signatures; it now
  passes `GFX_FONT_6X8`, and the demo builds and passes again.
- `pview.exe` and `gfxdemo.exe` predated the package-95 SDK (the
  per-program build unpacks the SDK once; the packed members carry
  zeroed dates, so the unpack is never refreshed).  Rebuilt against
  the current SDK, TestPView and TestGfxDemo pass.  The SDK archive
  was also rebuilt with the full libgfx member list (gfx_font4.c,
  gfx_atlas.c included).

## Validation

```sh
make && make sdk
python3 -m unittest tests.test_gfx tests.test_vblank tests.test_sim_core
make clean && make check
git diff --check
```

All green.  TestGfxTiming, TestGfxAtlas, TestGfxPages and the new
VdpClockUnit (two register writes exactly 48 T-states apart; an HMMM
of 2,560 bytes keeping CE busy for the model's 104,910 T-states)
pin the measured figures.
