# VGMPlayer for KonaOS

This is the native KonaOS C port of `Code/HOST/VGMPlayer`. It plays the same
compiled ZVGC v1/v2 streams on Afternoon Blend PSG0 and is built solely with
the standalone KonaOS 0.2.0 application SDK supplied by ZephyrOS.

## Build

From this directory:

```sh
make
```

The first build unpacks
`../../build/kos-sdk-0.2.0.tar.gz` beneath the ignored local
`build/` directory. The packed application is:

```text
build/user/vgmplay.exe
```

`KOS_SDK_ARCHIVE` and `KOS_SDK_ROOT` can be overridden for a different SDK
location or an already unpacked SDK.

## Compile music

The existing compiler produces the format accepted by both players:

```sh
python3 ../VGMPlayer/tools/compile_vgm.py music.vgm music.zvg
python3 ../VGMPlayer/tools/compile_vgm.py music.vgz music.zvg --loops 3
```

It extracts SN76489 `50h` writes, quantizes waits to the VGM metadata rate and
packs same-tick writes into ZVGC v2 runs. Rates from 1 through 180 Hz are
supported; a VGM rate of zero defaults to 60 Hz.

## Run

Copy `vgmplay.exe` and a compiled `.zvg` file onto a KonaOS filesystem:

```text
vgmplay /fs/MUSIC.ZVG
```

Press any key to stop. Every exit path stops and releases the timer event,
stops the stream, closes the file and releases `/dev/sound`; the driver's
release resets and silences all four PSG chips.

## Native runtime design

- The player owns no decoder. The file is wrapped in a borrowed
  `kos_reader_t` (`kos_reader_from_stdio`, absolute `fseek`/`fread` reads
  through the stream's own buffer) and handed to libsnd's pre-rendered
  stream player: `snd_zvg_open` validates the ZVGC header, reserves PSG0's
  four voices and adopts the header's 1-180 Hz playback rate.
- `/dev/timer` posts at the period `snd_timer_period()` derives from that
  rate. The event handler feeds `snd_tick` the delivered event count;
  coalesced events replay through libsnd's documented catch-up bound.
- libsnd's tick path renders the stream from a 256-byte ring: whole commands
  up to the next wait in one tick, the bytes to PSG0's actual port. No C
  runs on the tick; the handler performs no filesystem or console operations.
- The foreground loop tops the ring up with `snd_service()` (one bounded
  read per pass, 64 bytes at most), then either reads a keypress or calls
  `idle()` after an empty console poll. The CPU sleeps until the next
  interrupt instead of busy-polling while timer events drive playback.
- The stream's completion, malformed opcodes and reader errors all surface
  through `snd_zvg_get_status`: the loop exits on the DONE/ERROR state, the
  chip is muted, and the report prints the stream's ticks, consumed bytes,
  writes and underruns.

The port reports timer events, playback ticks, stream bytes, PSG writes,
ring underruns and missed events when playback ends. A clean run has zero
underruns and zero missed events.
