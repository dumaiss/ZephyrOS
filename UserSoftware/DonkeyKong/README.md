# DonkeyKongKos

DonkeyKongKos is a KonaOS-native re-host of the 16 KiB ColecoVision Donkey
Kong cartridge.  It is specifically designed to answer whether the unusable
sprites seen through the Coleco compatibility path are a 10 MHz timing issue
or a TMS9928/V9958 compatibility issue.

This is not another `ColecoGoKos` takeover.  It remains a KonaOS process,
loads the cartridge through the SDK's extended-memory API, owns `/dev/video`
through an exclusive device handle, subscribes directly to the V9958
vertical-blank event, and returns to the shell cleanly.

## What is native

- The V9958 runs in GRAPHIC 4: 256x212, 16-colour bitmap mode.
- The original TMS Graphics-II pattern, colour and name tables are maintained
  as a semantic shadow in upper V9958 VRAM.  Assembly code translates dirty
  tiles to the GRAPHIC 4 bitmap.
- Coleco four-byte sprite records are converted to V9958 sprite-mode-2 SAT
  and per-line colour records.  The relevant per-scanline limit is therefore
  eight sprites rather than the TMS9928's four.
- The original SN76489 software sequencer is retained and targets PSG 0 at
  Zephyr-80 port `E0h`.
- Keyboard input is translated directly to the controller fields consumed by
  Donkey Kong.  `WASD` moves, Space jumps/acts, and `Q` exits.
  Input bytes are ingested on every VDP event even when an expensive VRAM
  translation temporarily skips the non-reentrant game frame.

The standard Coleco BIOS is still required, but only as a source for its
well-tested software object, timer, writer, and sound engines.  At load time
the safe BIOS tail is placed at its original addresses and every VDP,
controller, random, and low-memory-conflicting entry is replaced by native
assembly.  No Coleco interrupt, TMS display mode, controller port, or BIOS
boot path is used.

The cartridge's ordinary NMI is represented by the V9958 vertical-blank
event.  DKKos subscribes through its exclusive `/dev/video` handle, selects
status register 0, and enables IE0 in VDP R#1.  KonaOS acknowledges the
physical interrupt and delivers the event callback, which invokes Donkey
Kong's original `852Eh` frame/NMI routine.  The interrupt reaches KonaOS on
`/INT`; it does not require the ColecoVision's NMI wiring.
Because event callbacks enter with the far-page field cleared, the callback
temporarily reopens the cartridge's `8000h` aperture before entering `852Eh`
and restores the primary event mapping before returning to libkos.
The retained OS-7 routines also depend on their low-memory parameter decoder
at `0098h`; DKKos relocates that decoder and patches all 17 call sites because
KonaOS owns native page zero.

The video takeover establishes a complete V9958 baseline rather than
inheriting the console's live state: it aborts any old command, selects the
actual G4 mode bits (`R#0=06h`), clears scroll/adjust/select registers, and
waits for the initial bitmap clear before allowing cartridge VRAM traffic.
Before IE0 is enabled it restores R#15 to status 0, and every two-byte VDP
control sequence is interrupt-atomic because the driver's status read resets
the VDP command latch.

## Build

The project is a sibling of the other KonaOS ports and builds only against
the packaged application SDK:

```sh
cd ZephyrOS/UserSoftware/DonkeyKong
make
make test
```

Output: `build/user/dkkos.exe`

Replacing `ZephyrOS/build/kos-sdk-0.1.0.tar.gz` is enough to make the next
build unpack and relink against the newer library.

## Run

Place the supported 16 KiB `DK.ROM` and standard 8 KiB `COLECO.ROM` in the
current KonaOS directory, then run:

```text
dkkos
```

Alternative paths may be supplied explicitly:

```text
dkkos /fs/games/dk.rom /fs/games/coleco.rom
```

The loader rejects other revisions.  The pinned images are:

- `DK.ROM`: 16384 bytes, CRC16-CCITT `0DC3`
- `COLECO.ROM`: 8192 bytes, CRC16-CCITT `5351` (standard BIOS CRC32
  `3AA93EF3`)

The original options screen is bypassed for this diagnostic build; it starts
one player at difficulty four.  All four arcade stages, scoring, timers,
animation, collision logic, and sound continue to come from the original game
and BIOS assembly.

## Interpreting the hardware result

The test deliberately changes the display architecture while preserving the
game logic and the 10 MHz CPU:

- If sprites are now stable, the old failure is in the TMS compatibility
  path—most likely its four-sprites-per-line behaviour or related status
  semantics.
- If the same gameplay/sprite fault remains, CPU-rate-sensitive logic is the
  stronger explanation.  The existing half-throw-rate cartridge patch can
  then be applied as a second, independent timing experiment.
- If the display is stable but motion cadence is wrong, the port has isolated
  a frame-delivery issue from a sprite-rendering issue.

## Current validation boundary

`make test` verifies the SDK-packed executable, the protected memory layout,
the exact ROM revisions, every hardware hook, and the 1-bpp/colour to G4
pixel expansion.  A production-image simulator replay has also exercised the
physical VDP interrupt model, event delivery, far-aperture restoration, and a
clean `Q` return through video release.  A real-card play-through is still
required; in particular, sprite-mode-2 placement and sustained performance
must be checked on the V9958 hardware before calling the diagnostic result
conclusive.
