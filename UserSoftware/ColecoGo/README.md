# ColecoGo for KonaOS

This is the native KonaOS port of `Code/HOST/ColecoGo`.  The loader is C and
uses the standalone KonaOS 0.1.0 SDK for arguments, files, console output and
diagnostic bank access.  A small Z80 assembly handoff is retained because the
last operation changes the memory map and intentionally never returns to the
operating system.

## Build

From this directory:

```sh
make
```

The first build unpacks
`../../../../ZephyrOS/kos-sdk-0.1.0.tar.gz` under the ignored `build/`
directory.  The application is written to:

```text
build/user/colecogo.exe
```

Override `KOS_SDK_ARCHIVE` or `KOS_SDK_ROOT` when the SDK is elsewhere.
Run `make test` to replay the final bank switch and Stage-B memory construction
against KonaOS's Z80/memory-decoder simulator.

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

## Handoff and bank use

All file reads and validation finish before the target bank is changed.  The
port uses bank 5 for the final ColecoVision personality and refuses to start if
KonaOS placed the loader itself there.  Bank 6 is left intact because KonaOS
uses it for resident drivers.

The SDK diagnostic API constructs bank 5 except for page zero, which that API
correctly protects.  The no-return handoff then disables KonaOS interrupt
sources, establishes the V9958/TMS-compatible state, switches to bank 5,
selects the flat-memory personality, installs BIOS page zero, finishes the
cartridge copy through `FFFFh`, clears Coleco RAM and jumps to `0000h`.

This is a hardware personality takeover, not a normal process exit.  Returning
to KonaOS is unsupported; reset the machine to leave a game.
