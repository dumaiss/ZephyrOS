# Validation - snd-ztr-orders (package 102c01)

Date: 2026-10-27

## Scope

Package 102c01: replace 101's C row decoder and 3-row record queue
with the order-buffer streaming model from `docs/snd-sdk.md` section
6.  The foreground copies raw pattern streams, one order per 1,024
-byte buffer; the assembly tick decodes each channel's due event
through per-channel cursors (the Zephyr-80 `player.asm` model).  No
OS change, no ZTR v1 change.

## Files

Changed:

- user/sdk/snd/src/snd_ztr.c - the producer: order double buffers
  (`snd_ztr_order_area[2]`), `setup_order_for` (the directory scan),
  `refill_order` (one read per stream into the buffer's cursor table
  plus packed streams), the published/consumed handshake,
  windowed open validation (255-byte windows, one reader call per
  window; the pattern streams walk one shared window over the whole
  pattern-data region), prepare/service/play/pause/resume/stop/close
- user/sdk/snd/src/snd_private.h - order-buffer constants
  (`SND_ZTR_ORDER_BYTES` 1024 = 64-byte table + 960-byte streams),
  the handshake globals; the queue/row/slot constants removed
- user/sdk/snd/src/snd_ztr_dec.asm - `_zt_take_row` decodes the raw
  events from the front buffer's cursors into the event slot and
  applies them through the existing engine; `_zt_advance_position`
  takes the published buffer at the order change or underruns
- tests/snd/sndtest.c - `ztr FILE timed [batch]`, shared timed
  plumbing with the ZVG player, ztrbudget markers, ztrload per-order
  buffer checksums, the stall mode re-pinned to order-change
  underruns
- tests/snd/host/host_ztr.c - the producer's host build: compares
  each order's buffered streams with the bytes `setup_order_for`
  selects, plus one stream checksum per order
- tests/snd/ztrloadkres.c - the KRES drain against the same checksums
- tests/test_snd.py - TestSndZtrLoad's host comparison (no more
  oracle row records), the stall pins, new TestSndZtrClient (timed
  mode and sntrack playback in the simulator)
- docs/sdk.md, docs/snd-sdk.md section 6.2

Removed:

- `snd_ztr.c`'s `decode_event`, `produce_row_record`,
  `ztr_produce_rows`, `ztr_window_complete`, the 3-row queue and the
  row-record addressing
- `snd_private.h`'s `SND_ZTR_QUEUE_ROWS`, `SND_ZTR_ROW_BYTES`,
  `SND_ZTR_SLOT_BYTES`
- `_zt_take_row`'s decoded-slot consumption and the slot-apply loop

## Results

All acceptance runs below used the final tree (the SDK archive and
sntrack rebuilt from it).

Producer (host):

- night-market and RunawayCircuit: every order's buffered streams
  byte-equal the file bytes the directory scan selects; 30 and 136
  per-order stream checksums; no backward reader offsets in the
  short-read and error modes; the error mode refuses at the second
  reader call (-10).
- 43/43 malformed fixtures refused at open.

Card + KRES:

- `sndtest ztrload` per-order checksums equal the host's, both songs,
  from the card file and through ztrkres (KRES bundle).

Engine gate:

- 3,000-tick state and write gate, DIRECT and BATCH, both songs:
  6,000 markers, 0 write mismatches, 0 state mismatches (TestSndZtr).
- Pause/resume/stop/ownership and the stop-song effect unchanged.
- Stalled reader: the song plays through the start order, underruns
  exactly once at the order change (mute, count, hold), and service
  resumes at that order's first row with audible writes.

Real playback (requirement 4):

- `sndtest ztr /fs/run.ztr timed`: night-market ends DONE with 11,518
  timer events for the oracle's 11,518 ticks (0 underruns).
- sntrack.exe in the simulator, both songs: 0 underruns, 0 overruns,
  0 missed events; timer events equal the song's tick count (11,518
  and 13,056) - within 2%; the writes equal the goldens for the
  first 3,000 ticks of each song.

## Budgets

| figure | measured | target | status |
|---|---|---|---|
| sntrack underruns, both songs | 0 | 0 | pass |
| tempo: timer events vs song ticks | +0 / +0 (0%) | within 2% | pass |
| `snd_ztr_open`, night-market | 13,948,737 T-states | <= 5 M | report (profile below) |
| `snd_ztr_open`, RunawayCircuit | 36,731,558 T-states | <= 5 M | report (profile below) |
| one order refill | 380,789 / 196,704 T-states | report | pass |
| ZTR tick, worst, % of a 60 Hz frame | 59,343 (36%) / 108,945 (65%) | report | report (profiles below) |
| OS bytes | unchanged | 0 | pass |
| sndtest.exe image + BSS | 36,331 + 20,360 + 256 = 56,947 | <= 56,960 | pass |

Open cost: above target.  The sndprof window-0 profiles attribute it
to the in-memory work of the open pass, not the reader calls:
night-market 8.7 M attributed (`_pattern_streams_ok` 3.8 M,
`_setup_order_for` 1.6 M, `_resident_macro_ok` 0.4 M);
RunawayCircuit 31.8 M attributed (`_setup_order_for` 21.5 M - the
largest-order scan runs the directory scan once per order,
`_pattern_streams_ok` 4.3 M).  Optimizing the open pass is deferred
by owner decision ("correctness first, optimize later").

Worst ticks, per sndprof (T-states, top functions):

- night-market tick 1668: 59,343 -
  `_zt_bind_one` 6,039, `_zt_macro_base` 4,067, `_zt_dec_loop` 4,008,
  `_zt_rm_loop` 3,500, `_zt_am_due` 3,409
- RunawayCircuit tick 2628: 108,945 -
  `_zt_bind_one` 8,723, `_zt_macro_base` 7,719, `_zt_rm_loop` 7,000,
  `_zt_am_due` 6,350, `_zt_bi_loop` 5,297

Tick medians across the 3,000-tick windows: 17,139 and 11,133
T-states (10% and 7% of a frame).

## Notes

- The engine's cursor decode ports `player_begin_row`,
  `player_decode_channel_row` and `player_setup_order` from the
  Zephyr-80 assembly tracker; the C oracle (`tests/snd/oracle`)
  remains the semantic reference.
- Two bugs fixed during validation: the decode's event-defaults loop
  was clobbering the stream cursor (HL), and validate passed the end
  of the counts scratch instead of its base after the directory
  loop's pointer walk.
