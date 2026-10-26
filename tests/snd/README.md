# The sound-series oracles and harness

Package 97 froze the behaviour libsnd must reproduce: golden register
traces of the two working players, a host build of each as an oracle,
and the shared replay harness every later package proves itself
against.

## Tools

| Path | What it is |
| --- | --- |
| `oracle/ztr_trace.c` | a host build of the preserved `oracle/ztr.c` + `player.c` + `psg.c` with a recording `sound_write`. Prints the per-tick trace for N ticks and optionally the 16 channels' state. |
| `oracle/zvg_trace.c` | a self-contained host reference of the ZVG decoder: the same header checks, command ring and per-tick `decoder_tick` the libsnd implementation and the goldens pin, fed whole files. Prints the per-tick byte sequence for PSG0. |
| `oracle/make_malformed.py` | runs both oracles over `fixtures/malformed/` and prints the outcomes table. |
| `tests/test_snd_oracle.py` | builds the tools, pins the goldens byte for byte, pins the malformed table and replays the harness self-test. |
| `sndprobe.c`, `sndprobe_delay.asm` | the harness self-test program (a card program; `make sndprobe`). |

The oracle tools build with the host compiler into `build/host/snd/`
(the test builds them itself):

```sh
cc -std=c99 -Wall -Wextra -Werror -Itests/snd/oracle/include \
   -IUserSoftware/SNTracker \
   tests/snd/oracle/ztr_trace.c tests/snd/oracle/ztr.c \
   tests/snd/oracle/player.c tests/snd/oracle/psg.c \
   -o build/host/snd/ztr_trace
cc -std=c99 -Wall -Wextra -Werror -Itests/snd/oracle/include \
   tests/snd/oracle/zvg_trace.c -o build/host/snd/zvg_trace
```

The `oracle/include/kos/` stubs are the minimal host surface the two
players include; they are not the real SDK headers.

## Trace format

`golden/*.trace` is text, one line per tick:

```text
# comment
tick N: c<chip>:<hh> ...
```

`chip` is 0-3 (the sound targets) and the bytes are the writes the
player made that tick, in issue order.  The ZTR traces cover 3,000
ticks of each song; the ZVG traces the whole stream.  The ZTR player's
register cache suppresses unchanged bytes, so a tick can be empty.

## The malformed table

`golden/malformed.md` pins, for every fixture in `fixtures/malformed/`,
the stage the oracle reached (`ztr-load`, `zvg-open`, `zvg-decode`) and
its outcome line.  Regenerate after an oracle change with:

```sh
python3 tests/snd/oracle/make_malformed.py > tests/snd/golden/malformed.md
```

## Regenerating the goldens

```sh
build/host/snd/ztr_trace tests/snd/fixtures/night-market.ztr 3000 \
    > tests/snd/golden/night-market.trace
build/host/snd/ztr_trace tests/snd/fixtures/RunawayCircuit.ztr 3000 \
    > tests/snd/golden/RunawayCircuit.trace
build/host/snd/zvg_trace tests/snd/fixtures/run-shortwaits.zvg \
    > tests/snd/golden/run-shortwaits.trace
build/host/snd/zvg_trace tests/snd/fixtures/wait16.zvg \
    > tests/snd/golden/wait16.trace
```

Only regenerate the goldens for a deliberate behaviour change in one of
the two reference players; the goldens are the contract, not the
oracles' scratch output.

## The harness (`snd_harness.py`)

The sound series' replay harness, shared by packages 98-103:

- **replay** - `SndReplay.start(files, command)` boots the production
  image with the emulated Afternoon Blend card (`z80sim.SoundPeer`)
  and the fixtures on the card, then runs a program from the shell;
  `SndReplay.collect` returns the `(port, value, T-state)` writes.
- **split** - `split_ticks(writes)` cuts the writes into per-tick lists
  at the **DAC marker**: port E4h, value A5h, which no song writes.  A
  program emitting ticks writes the marker once before each tick's
  bytes.  Writes before the first marker are orphans; writes after the
  last are the trailing batch (for example a release's silence).
- **compare** - `compare_ticks(ticks, golden, chip_map)` matches the
  split ticks against a golden trace; a mismatch names the tick and the
  first differing byte.  `chip_map` moves a golden's chip (a ZVG played
  on another PSG).
- **timing** - `SndReplay.profiled` steps one instruction at a time and
  attributes each step's T-states by PC: only the program's code window
  (0100h-1FFFh, the mode-11 latch-bank region) counts, so the kernel's
  interrupt path (the E000h vector page and the bank-7 code at
  2000h-5FFFh) is excluded - the same attribution `tests/gfxprof.py`
  uses.  It returns the per-interval totals and the markers' T-states.

The self-test is `sndprobe`: three marker-ticked byte sequences, then
two calibrated delays (30 + 13·n T-states each) framed by markers, so
the harness's split, compare and timing all decode exactly.
