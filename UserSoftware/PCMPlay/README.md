# PCMPlay for KonaOS

A KonaOS PCM player for the Afternoon Blend's AD7801 DAC: 8-bit mono
samples streamed from the card, timed by the real-time interrupt. It is a
thin client of libsnd's PCM player (`snd_pcm_*`, docs/snd-sdk.md section
12), which holds the format, the handler, the decoder and the timing. It
needs IO controller firmware level 76 or later.

- `src/pcmplay.c` - the player.
- `tests/test_pcmplay_sim.py` - the player on the production image in the
  simulator, both codecs.
- The converter is the SDK's `tools/pcmconv.py`.

## Build and test

```sh
make          # build/user/pcmplay.exe
make test     # the simulator replays
```

The replays use ZephyrOS's `build/konaos.bin` and `tests/` harness and are
skipped without them.

## Convert

```sh
T=build/kos-sdk-0.2.0/tools
python3 $T/pcmconv.py "West of House.wav" -o westhouse.pcm --title "West of House"
python3 $T/pcmconv.py music.mp3 -o music.pcm --normalize --codec dpcm4
python3 $T/pcmconv.py music.pcm -o music4.pcm --codec dpcm4   # exact re-encode
python3 $T/pcmconv.py --info music.pcm
python3 $T/pcmconv.py --to-wav music.pcm -o check.wav         # exact DAC bytes
```

FFmpeg (through `subprocess`) decodes, averages the channels to mono and
resamples with its anti-alias filter (SoX's resampler when FFmpeg has
it). The converter applies the gain (`--gain DB`, or `--normalize` for a
two-pass peak scale), TPDF dither of one LSB (`--dither none` to disable;
seeded by `--seed`, so output is reproducible), rounding and clipping.
`--rate` takes 4,000-24,000 Hz (16,000 by default). `--codec dpcm4` stores
the samples losslessly in about half the bytes. Titles are stored in
CP850; characters the console cannot show become `?` with a warning.

## Run

Copy `pcmplay.exe` and a `.pcm` file to the card:

```text
pcmplay advent.pcm
pcmplay -l advent.pcm          loop until a key
pcmplay -g 24 advent.pcm       another lane gap (us; default 20)
pcmplay -i advent.pcm          header only; nothing plays
```

It prints the title, length, codec and rate, plays at once, updates a
progress line every second and stops at the end or on any key (polled
once a second; the IOC holds the key). The exit report:

```text
Samples 480000  underruns 0  rate 16000 Hz
Service 46% of the playback (118 reads)
Elapsed 27835 ms  loops 0
```

- `underruns` - ticks replayed from an empty ring (a page each).
- `Service` - the share of the playback the foreground spent in
  `snd_pcm_service` (card reads and the codec 1 decode), measured in RT
  ticks; what remains is the program's.
- `Elapsed` - the system timer's view, which loses ticks while a card read
  runs masked.

Exit status: 0, 1 usage, 2 file or header, 3 read error, 5 sound card or
real-time interrupt unavailable (another program holds it, or the IO
controller has no RT clock), 6 the file ended early.

While PCMPlay plays, `/system` and `reboot` are busy (the ROM mode serves
no NMI).
