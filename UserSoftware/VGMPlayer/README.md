# VGMPlayer for KonaOS

This is the native KonaOS C port of `Code/HOST/VGMPlayer`. It plays the same
compiled ZVGC v1/v2 streams on Afternoon Blend PSG0 and is built solely with
the standalone KonaOS 0.1.0 application SDK supplied by ZephyrOS.

## Build

From this directory:

```sh
make
```

The first build unpacks
`../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz` beneath the ignored local
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
closes the file and silences all four voices on all four PSG chips.

## Native runtime design

- `/dev/timer` posts at the original player's approximately 180 Hz base rate.
  A phase accumulator derives the ZVGC header's requested playback rate.
- Two alternating 6144-byte buffers are filled through the SDK's `FILE`,
  `fopen()` and `fread()` streaming interface. Runtime refills transfer only
  512 bytes per foreground pass.
- Each application refill is requested as two 256-byte `fread()` items so it
  passes through the SDK's measured 512-byte stream buffer. The player enables
  `fmeasure()`, reports `fstats()`, and feeds the measured mean/worst refill
  times plus the ZVGC header's encoded-byte rate into `stream_plan()`.
- Foreground code copies stream bytes into a 256-byte ring. Atomic byte-sized
  head and tail indices let the event handler consume complete commands safely.
- Timer events coalesce while a filesystem call is active. The handler replays
  the delivered count, which is the KonaOS equivalent of the original
  player's pending-CTC-tick mechanism.
- At startup and shutdown, the player writes the four volume-off commands to
  PSG0 through PSG3 (ports `E0h` through `E3h`). The chips have no hardware
  reset and an undefined power-on state, so this prevents unused PSGs from
  producing stray tones. Music playback itself remains routed to PSG0.
- The handler performs no filesystem or console operations. PSG0 writes use
  the SDK's `diag_io_out()` wrapper, the same application mechanism used by
  KonaOS's `beat.exe` hardware diagnostic because KonaOS 0.1.0 does not yet
  publish a sound driver.

The port reports base timer events, playback ticks, PSG writes, ring underruns
and missed events when playback ends. A clean run has zero underruns and zero
missed events.
