# ColecoGo for KonaOS

This is the native KonaOS port of `Code/HOST/ColecoGo`.  The loader is C and
uses the standalone KonaOS SDK for arguments, files, console output and the
ColecoVision memory: extended-memory pages and the `personality` handoff on
Zephyr-80 Mk2, diagnostic bank access on Zephyr-80.  Small Z80 assembly
routines set the V9958 up for the ColecoVision (`video.asm`) and, on
Zephyr-80, switch the memory latch (`handoff.asm`); the last operation never
returns to the operating system.

## Build

From this directory:

```sh
make
```

The first build unpacks the SDK of the selected board (`BOARD=mk2`, the
default, or `BOARD=mk1`; `../sdk.mk`) into the ignored local `build/`
directory; `make clean` before changing board.  The application is written to:

```text
build/user/colecogo.exe
```

Override `KOS_SDK_ARCHIVE` or `KOS_SDK_ROOT` when the SDK is elsewhere.
Run `make test` for the board's simulator tests: on Mk2 the production image
boots with `colecogo.exe`, a synthetic BIOS and cartridge on the card, and the
launch must reach the cartridge entry with the ColecoVision space mapped and no
KonaOS interrupt left (`tests/test_personality.py`); on Zephyr-80 the final
bank switch and the Stage-B memory construction are replayed
(`tests/test_handoff.py`).

## Run

Place `colecogo.exe`, an exact 8192-byte standard ColecoVision BIOS named
`COLECO.ROM`, and a cartridge of at most 32768 bytes on the KonaOS filesystem.
From the directory containing the BIOS, run:

```text
colecogo GAME.ROM
```

KonaOS passes only the command tail in `argv`, so `GAME.ROM` is `argv[0]`.
The optional patch manifest is derived from the cartridge path by replacing
its extension with `.PAT`; its `CGP1` format is compatible with the CP/M
ColecoGo tools.

## Handoff and memory

All file reads and validation finish before the target memory is changed.

On Zephyr-80 Mk2 the ColecoVision space is eight 8 KiB extended-memory pages:
the adapted BIOS at `0000h`, the open expansion area (`FFh`) at
`2000h-5FFFh`, the cleared RAM at `6000h-7FFFh` and the cartridge with its
`FFh` fill at `8000h-FFFFh` - one seven-page object and one one-page object,
the largest an object may be.  The program then sets the V9958 up, with
interrupts masked, and calls `personality`: KonaOS stops its interrupt and NMI
sources (the RT clock, the sound card, the CTC), maps the eight pages as the
whole memory and jumps to the BIOS at `0000h`.

On Zephyr-80 the port uses bank 5 for the final ColecoVision personality and refuses to start if
KonaOS placed the loader itself there.  Bank 6 is left intact because KonaOS
uses it for resident drivers.

The SDK diagnostic API constructs bank 5 except for page zero, which that API
correctly protects.  The no-return handoff then disables KonaOS interrupt
sources, establishes the V9958/TMS-compatible state, switches to bank 5,
selects the flat-memory personality, installs BIOS page zero, finishes the
cartridge copy through `FFFFh`, clears Coleco RAM and jumps to `0000h`.

This is a hardware personality takeover, not a normal process exit.  Returning
to KonaOS is unsupported; reset the machine to leave a game.
