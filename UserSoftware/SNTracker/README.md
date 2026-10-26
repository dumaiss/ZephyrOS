# SNTrackerKos

SNTrackerKos is the KonaOS ZTR player UI. Playback is provided by SDK 0.2.0's
`libsnd`: the application wraps an ordinary `FILE *` in a `kos_reader_t`,
validates and prepares the song, calls `snd_tick()` from its timer handler, and
keeps the prepared-row queue filled with `snd_service()` in the foreground.
Patterns stream from the file; the player does not load the complete song into
RAM, so valid ZTR v1 files may use the format's 65,535-byte limit.

The status line shows the current order, row and underrun count. Press `Q` or
Escape to stop; every exit path silences the song and releases `/dev/sound`.

## Build

From this directory:

```sh
make
```

The Makefile extracts and links against
`ZephyrOS/build/kos-sdk-0.2.0.tar.gz`. The executable is
`build/user/sntrack.exe`. `make test` checks the client integration and
validates the packed executable.

## Run

```text
sntrack night-market.ztr
```

CSM and Furnace authoring tools remain in `tools/`; example songs are in
`songs/`.
