# KonaOS Drivers

The resident drivers: what each one does, the protocol it serves, and how
the drivers are wired together. The rules are in `docs/architecture.md`
section 5; the image format is `docs/module-format.md`; the protocols'
numbers and frames are `abi/kos/protocol.h`.

## 1. The model

```text
driver image    one bank-6 image: its segments, its endpoints, the /dev names it
                publishes, its dependencies (docs/module-format.md)
endpoint        a (class, instance) entry with a host-assigned number
publication     a name under /dev bound to an endpoint and a protocol type
dependency      a peer endpoint a driver needs, resolved by the host to a number
                the driver reads from its boot bundle
```

Nothing registers at run time. The host packer numbers the endpoints,
resolves dependencies and builds the publication table; the loader installs
them before any driver runs. A driver reaches a peer only by calling the
endpoint number in its boot bundle, never by name. An unsupported operation
fails with a negative error; nothing is inferred from a name.

The production store carries the drivers below, loaded in this order:

| driver | class | serves | publishes |
|---|---|---|---|
| `sio` | `SERIAL` (1) | the serial port | `/dev/serial/0` |
| `ioc` | `IOC` (5) | the Z80-to-MCU transport | `/dev/ioc` |
| `hid` | `HID` (4), type `INPUT` | keyboard text over IOC | `/dev/input/keyboard` |
| `v9958` | `VIDEO` (2) | the V9958, its terminal renderer | `/dev/video` |
| `console-v9958-hid` | `CONSOLE` (6) | the console, over HID and V9958 | `/dev/console` |
| `iocfat` | `FILE` (7), types `FILE` and `FS` | the filesystem over IOC | `/fs` |
| `timer` | `TIMER` (10) | the monotonic clock, CTC physical channel 2 (port 41h) | `/dev/timer` |

The serial build swaps `console-v9958-hid` for `console-serial`.

## 2. The protocol frame

Every operation is one call with the 32-byte frame:

- `arg0`, `arg1`: the operation's two words;
- `payload`: inline bytes;
- `desc`: the nucleus-issued descriptor id, or `KOS_IPC_DESC_NONE`;
- `status`: the result, 0 or positive, or a negative error.

A descriptor-bearing operation passes `arg0` = offset into the caller's
buffer and `arg1` = length, so one call moves one bounded run. Operations
that answer a single word return it as the call's status.

Drivers pass work down a chain without staging: a driver that is called
with a descriptor can pass the descriptor id to a peer, and the driver that
holds the data copies straight to the original caller. Driver-to-driver
calls are near-direct: the frame travels by reference and no bank switch
happens. A driver that has bytes to hand on can also name its own
`DRIVER_IO` buffer in the reply payload for the peer to copy from.

## 3. SIO (`drivers/sio/`)

SIO channel B, the same channel as the early console, as a polled driver:
it owns no IRQ, so the early console keeps working before and without it.

- Ports: data `22h`, control `23h`; 115,200 8N1; `WR1 = 00h`.
- Init repeats the early console's programming
  (`18h, WR4 = 44h, WR3 = C1h, WR5 = EAh, WR1 = 00h`); it is idempotent.
- Bounded polling: `putc` waits for TX-empty with a finite budget
  (`KOS_SIO_TX_TIMEOUT`) and returns the timeout on expiry; `write` returns
  the bytes written (a partial count if a timeout follows the first byte).

| op | number | frame | result |
|---|---|---|---|
| `KSIO_OP_PUTC` | 0 | `arg0` = byte | 0 or `-KOS_ETIMEDOUT` |
| `KSIO_OP_WRITE` | 1 | desc in, offset, length | bytes written |
| `KSIO_OP_POLL` | 2 | — | 1 when a byte is pending, else 0 |
| `KSIO_OP_GETC` | 3 | — | byte 0-255, or `-KOS_EAGAIN` |
| `KSIO_OP_READ` | 4 | desc out, offset, length | bytes drained |

## 4. IOC (`drivers/ioc/`)

The Z80-to-MCU transport over both channels of SIO1: channel B carries
commands and replies, channel A carries bulk data. **It is transport
only**: it knows nothing of names, files, FAT, paths or caching. The MCU
firmware is unchanged.

**Headers.** `drivers/include/ioc.h` is the transport vocabulary;
`drivers/include/ioc_protocol.h` names the MCU's command and reply classes
and DATA offsets that clients share (PING, HID input fetch);
`drivers/ioc/ioc_proto.h` is private to the IOC driver (the bulk
READY/DONE constants, the bulk-arming class gate, controller status bytes).

**Frames.** A client sees `struct kioc_frame`: TYPE, STATUS, DATA length
and up to 26 DATA bytes. On the wire one frame is
`A5 5A LEN_LO LEN_HI TYPE SEQ STATUS DATA... CRC`, with CRC-16-CCITT (poly
`1021h`, preset 0, MSB first, no final XOR). The sequence and the CRC are
private to the driver. A reply must match: `reply = request | 80h`, or the
explicit unknown-class reply `FEh`. `KIOC_BULK_MAX` is 512 DATA bytes.

**Lane setup** (once, at init):

- command lane: channel reset, `WR1 = 00h`, external sync x1
  (`WR4 = 30h`), `FFh` underrun fill (`WR7`), receiver off (`WR3 = C0h`),
  transmitter on with RTS inactive (`WR5 = 68h`);
- bulk lane: channel reset, `WR1 = 00h`, `WR4 = 30h`, `AAh` fill (`WR6`,
  `WR7`), receiver on without Hunt (`WR3 = C9h`), transmitter idle
  (`WR5 = E0h`).

A channel reset, a receiver disable or Enter Hunt destroys character
synchronization, so an ordinary transaction never issues one; the bulk lane
enters Hunt only before its first verified packet and during recovery.

**Link bring-up and recovery.** Up to four `CMD_LINK_SYNC` attempts, each
clearing both lanes' sync flags and re-arming Hunt. The link is up only
after a CRC-verified `RSP_LINK_SYNC`. The same bounded sequence recovers the
whole link after any bulk-lane fault. A link that is down at init does not
fail the driver: the next ordinary operation recovers it.

**One command.** Malformed or oversize frames are refused before any lane
I/O. The driver waits (bounded) for COMMAND_READY on `/DCDB`, stamps the
sequence, builds the packet, preloads `A5`, resets the EOM latch, asserts
RTS, streams the frame with interrupts masked, adds two `FFh` trailers,
releases RTS by disabling the transmitter, and arms the receiver before
restoring the interrupt state. The receive side clears the error latch
without resetting the channel, drains at most the three-byte FIFO, scans a
bounded window for `A5 5A`, validates header, body, CRC and sequence,
clears unused reply bytes and leaves the receiver on.

**Bulk admission (READY/DONE).** A reply to a request class that can arm
the bulk lane, shaped exactly like a bulk READY (status 0, length 8, a
non-zero transfer id, a known direction, 1-512 DATA bytes), admits one bulk
phase. The class gate matters: some ordinary replies are also eight bytes
long, so shape alone never changes the phase. While admitted, only the
matching `bulk_read` or `bulk_write` runs; anything else is `KIOC_EBUSY`
before any lane I/O. A completed read returns to idle. A completed write
must be confirmed by `CMD_XFER_STATUS` reporting the same transfer id and
success; a failed or mismatched DONE is never reported as a completed write
and arms recovery.

**Bulk read:** keep the entry interrupt state; clear the error latch; Hunt
only while the boundary is unproven; drain the FIFO; assert RTS; admit on
`/DCDA`; scan for `A5 5A`; capture the header at wire speed; move the
admitted length with the proven `INI` loop under a whole-transfer stall
budget; consume the two trailer bytes (the deployed firmware does not check
bulk DATA CRC in either direction); release RTS; confirm `/CTSA` is
released; then validate the header against the stored READY.

**Bulk write:** build the header first; program the `AAh` fill and arm the
transmit CRC generator; preload a `00h` lead-in; admit on `/CTSA`; send
`A5 5A`, the header and the payload with the proven `OUTI` loop; reset EOM
after the last byte; wait for the underrun; keep the measured 128-iteration
CRC completion guard; release RTS; restore interrupts before the possibly
long controller commit wait; confirm `/CTSA` is released.

**Failures.** Every timeout and malformed packet releases RTS, restores the
entry interrupt state, clears the busy guard and the admission, and arms
whole-link recovery, so no stale READY survives a fault. A private busy
guard refuses overlapping use with `KOS_EBUSY`.

| op | number | frame | result |
|---|---|---|---|
| `KIOC_OP_COMMAND` | 0 | `arg0` request type, `arg1` DATA length (0-24), `payload` DATA | 0; reply: `arg0` type, `arg1` DATA length, `payload[0..1]` reply-frame address, `payload[2]` STATUS |
| `KIOC_OP_REPLY` | 1 | desc out, offset, length | bytes copied from the stored reply |
| `KIOC_OP_BULK_READ` | 2 | `arg1` length (1-512); desc out, or `payload[0..1]` a hot-tier buffer of the caller's | the length |
| `KIOC_OP_BULK_WRITE` | 3 | `arg1` length (1-512); desc in, or `payload[0..1]` a hot-tier buffer of the caller's | the length |
| `KIOC_OP_STATUS` | 4 | — | the semantic status word (`KIOC_STATUS_*`) |
| `KIOC_OP_READY` | 5 | — | 1 when the link is up and no recovery is pending |

`KIOC_STATUS_HID_PENDING` is sampled from `/CTSB`; raw SIO status never
leaves the driver. Transport errors are the negative `KIOC_E*` codes
(`EBADFRAME`, `EBADCRC`, `EBADSEQ`, `ETIMEDOUT`, `EBUSY`, `EINVAL`, `EIO`).

## 5. HID (`drivers/hid/`)

Keyboard text over IOC. Its IOC dependency is declared in the manifest and
resolved by the host; it includes only the public IOC vocabulary, never the
IOC driver's private header.

- A 16-byte local ring. `poll`, `getc` and `read_text` serve buffered bytes
  first; otherwise they sample `KIOC_STATUS_HID_PENDING` and issue exactly
  one `CMD_HID_INPUT` only while it is set, asking for exactly the ring's
  free space (at most 24 bytes), because the controller dequeues what it
  sends.
- A reply is validated (transport success, `RSP_HID_INPUT`, controller
  status, 2-26 DATA bytes, a count within the request and the ring) before
  any byte is committed; a bad reply leaves the ring unchanged. Bytes are
  kept verbatim, NUL, control bytes and VT100 key sequences included.
- A set doorbell followed by a failed or empty fetch imposes a 64-call
  penalty before the next fetch; data or a released doorbell clears it. A
  transport failure reads as "no input", so a broken link cannot wedge
  console polling.

| op | number | frame | result |
|---|---|---|---|
| `KHID_OP_READ_TEXT` | 0 | desc out, offset, maximum | bytes transferred (0 when empty) |
| `KHID_OP_POLL` | 1 | — | 1 when a byte is buffered |
| `KHID_OP_GETC` | 2 | — | byte, or `-KOS_EAGAIN` |
| `KHID_OP_READY` | 3 | — | 1 when the IOC link is up |

## 6. V9958 (`drivers/v9958/`)

The V9958 video core with an 85×26 terminal renderer, so the same hardware
serves graphics clients and the text console.

**Terminal.** 85 columns × 26 rows of 6×8 cells on a 208-line surface
inside the circular 256-line page-zero bitmap, with precolored normal
(`10000h`) and reverse (`14000h`) font atlases, circular scrolling by
advancing R#23, and a sprite cursor. Printable bytes `20h-FFh` (CP850)
print. Control handling:

- CR, LF/VT, BS, TAB (8-column stops), FF and `ESC[2J` (clear and home);
- erase: `ESC[K`, `ESC[0K`, `ESC[1K`, `ESC[2K`, `ESC[J`, `ESC[0J`, `ESC[1J`,
  `ESC[X`, with the current background;
- `ESC[7m`, `ESC[27m`, `ESC[0m` (reverse/normal; other SGR parameters are
  consumed);
- `ESC[s`/`ESC[u` and `ESC 7`/`ESC 8` (save/restore cursor and rendition);
- `ESC[?7h/l` (auto-wrap), `ESC[?25h/l` (cursor visibility);
- `ESC[A/B/C/D/G/d/H/f` (cursor movement, clamped), `ESC[L`/`ESC[M`
  (insert/delete lines);
- `ESC c` and the triple-ESC reset re-initialize terminal and display;
- other control bytes and unknown or partial sequences are consumed
  without writing outside the terminal state or the bitmap.

Print runs batch up to 64 glyphs and flush before control operations. The
renderer runs on a private 256-byte stack, because its call chain is deeper
than the gate stack.

**Native operations** (through `KVIDEO_OP_SEND`): `KVIDEO_CTRL_WRITE` (1),
`DATA_WRITE` (2), `DATA_BLOCK` (3), `PALETTE_WRITE` (4), `INDIRECT_WRITE`
(5), `PRESENT` (6), `RESET` (7). Validation comes before any I/O: write
operations need a non-empty payload and stream the full 16-bit length;
`PRESENT` and `RESET` need length 0; unknown operations are unsupported.

The endpoint declares `"release": true`, so it also serves the common
`KOS_OP_ACQUIRE` and `KOS_OP_RELEASE` (section 14). While an exclusive
handle is held the console's output is dropped and counted instead of
rendered (the caller still gets its whole length back), and the release
re-runs the whole bring-up below. `KVIDEO_OP_RESET` (3) runs that same
bring-up at any time, and `ESC c` and the triple-ESC reset reach it through
the terminal parser.

**Frame event.** The endpoint also declares `"events": true`: the program
holding the VDP exclusively can `notify` its handle and receive the
V9958's frame (vertical-blank) interrupt as a device event.
- The interrupt reaches the CPU on `/INT` (the configuration latch's D0 =
  0, which the driver keeps) as the source `IRQ_VDP_FF`, because the VDP
  supplies no IM2 vector.
- The exclusive hold claims that source. The subscription selects status
  register 0, the TMS9918-style status the program reads.
- The handler reads status register 0, which clears the frame flag and
  releases `/INT`, then posts the event. The event is delivered when the
  interrupt returns into the program, so a loop that never calls the OS
  still receives every frame.
- The program enables the interrupt itself (IE0, R#1 bit 5), as on a
  ColecoVision. A second subscription, or one without the exclusive hold,
  is `-KOS_EBUSY`.
- Reading status resets the command port's two-byte latch, so the
  program's own register writes must not be split by the interrupt: make
  them from the event handler, or masked. While the frame event is on,
  the driver masks its own register writes and selects status register 2
  only around its command-engine polls.
- The release runs the display bring-up, which turns IE0 off and selects
  status register 2, before the claim is dropped.

**Hardware** (ports `A0h-A4h`, constants in
`platform/zephyr80/public/hardware.h`): the bring-up sets R#25 (WTE = 0,
VDS = 0), R#8 = `08h` (64K×4 DRAM), stops any command, loads the GRAPHIC 6
register state (R#0 = `0Ah`, R#2 = `1Fh`, R#9 = `88h`, R#15 = 2, R#23 =
`100h - 4`, ...), then R#25 = `04h` and the porch. The palette is
programmed under WAIT; both atlases are uploaded, the bitmap is cleared to
the `44h` background, the cursor set up, the frame presented and the
display enabled. Status register 2 stays selected while the driver owns
the VDP, and commands wait for CE. The bring-up leaves the frame interrupt
off; `IRQ_VDP_FF` is claimed only during an exclusive hold (the frame
event above).

**Font.** The 2,048-byte CP850 6×8 font (`assets/font_cp850_6x8.inc`) is
emitted by `tools/fontpack.py` into the driver's bulk segment, never into
its hot image. The driver converts it into the two atlases in VRAM.
`KVIDEO_RESET` re-runs the whole bring-up, reading the font through
`bulk_read`, because the boot/bulk tier is not mapped in the driver window.

| op | number | frame | result |
|---|---|---|---|
| `KVIDEO_OP_SEND` | 0 | `arg0` = native operation; desc in, offset, length for the streaming ones | 0 or negative |
| `KVIDEO_OP_WRITE` | 1 | desc in, offset, length: bytes for the terminal | bytes consumed |
| `KVIDEO_OP_PUTC` | 2 | `arg0` = byte | 0 |
| `KVIDEO_OP_RESET` | 3 | — | 0, or `-KOS_ENODEV` |

## 7. Console (`drivers/console/`)

One source, two images, selected per build:

| image | input | output |
|---|---|---|
| `console-v9958-hid` | HID | VIDEO |
| `console-serial` | SERIAL | SERIAL |

- The console forwards each operation as one call to the endpoint its
  build selected; a descriptor travels down unchanged, so nothing is
  staged here. It owns no buffer, no IRQ and no state beyond its two
  endpoint numbers.
- Its dependencies are required: a missing one fails its init, and a
  failed required driver is a boot failure. There is no run-time probe and
  no fallback.

| op | number | frame | result |
|---|---|---|---|
| `KCONSOLE_OP_WRITE` | 0 | desc in, offset, length | bytes written |
| `KCONSOLE_OP_READ` | 1 | desc out, offset, length | bytes pending, without waiting |
| `KCONSOLE_OP_GETC` | 2 | — | byte, or `-KOS_EAGAIN` |
| `KCONSOLE_OP_POLL` | 3 | — | 1 when input is pending |
| `KCONSOLE_OP_PUTC` | 4 | `arg0` = byte | 1, or negative |
| `KCONSOLE_OP_RESET` | 5 | — | 0 |

`KCONSOLE_OP_RESET` is the console's reset: it drains the input driver's
queue, so nothing typed for the program that is going away reaches the one
that starts, and the V9958 build then forwards `KVIDEO_OP_RESET` to its
output dependency (the serial build has no video to reset and answers 0).
The boot launcher runs it before every start of the boot program
(section 14).

After the drivers load, the boot prints `console: driver OK` through it.

**Five console paths**, not to be confused:

- the native VIDEO operations: graphics, independent of any text mode;
- V9958 character output: the terminal renderer, the production console's
  output;
- HID character input: the production console's input;
- the serial console: both directions over SIO, in the serial build;
- the early console: direct SIO for the banner, panics and failures,
  independent of every driver.

## 8. IOC-FAT (`drivers/iocfat/`)

The filesystem behind `/fs`: it serves the `FILE` protocol on open files and
the filesystem-namespace protocol (`FS`) on names, over IOC. See
`docs/filesystem.md`.

**No media.** A card that is absent, removed or unreadable does not fail the
driver (that would be a boot failure, and `/system` must stay usable). The
driver publishes `/fs`, reports the card state once, and answers
`-KOS_ENODEV` to every operation (`bind`, `stat`, `ns_enum`, `chdir` and the
FILE protocol). The card state is decided by opening the root **and
reading it**: the firmware answers the root open from its mount context
without touching the card, so only the first read proves the card is there.
`KFS_OP_STAT` on the root proves it the same way.

**Cache and read-ahead.** Reads go through a ring of four consecutive
blocks in the driver's `DRIVER_IO`: the bulk receive fills it, and each run
of consecutive blocks reaches the caller in one `copy_out`. A read that
continues where the previous one ended fills the whole ring (read-ahead),
unless a change dropped the ring since its last fill (reads between writes,
as in a copy); any other fills only the blocks it needs. When the firmware
offers `FS2_READ_RUN` (CAPS flag `1000h`), a fill of several blocks of an
uncached file is one run: one command, then one 512-byte bulk packet per
block, received back to back into the ring by `KIOC_OP_COMMAND_READ` (all
of them, or the error; a failed packet re-syncs the link). Without the
flag every block is its own `FS2_READ`. Blocks of files opened read-only
that fit it whole are also kept in a 16 KiB cache (32 blocks) in the
driver's bulk extent (a larger file is a stream and would only cycle
through it),
keyed by the path as opened and the size that open reported; a hit never
touches the IOC link. Coherence is coarse and safe: an open for writing, a
write, an unlink, a rename or a stale-media answer drops the whole cache.

## 9. TIMER (`drivers/timer/`)

The monotonic clock.  CTC physical channel 2 - the port the board's
crossed select wiring answers at `41h`, its IM2 entry `04h`, its trigger
input 7.3728 MHz - free-runs at 156.25 Hz (the 10 MHz system clock
through the 256 prescaler, time constant 250) and the driver's interrupt
handler counts the ticks; it is the first production interrupt source.  The driver publishes `/dev/timer` and serves the
`TIMER` protocol (class 10):

- `KTIMER_OP_NOW` (0), descriptor-less: `payload[0..3]` of the reply is
  the current count in 25.6 µs units, little-endian.  The driver reads
  the tick count, the channel's down-counter and the tick count again,
  retrying when the tick moved in between, so readings are monotonic;
  the count wraps after about 30.5 hours and elapsed times are unsigned
  differences.
- `KTIMER_OP_INFO` (1), descriptor-less: `payload[0..3]` is counts per
  10 s (390,625) and `payload[4..7]` counts per tick (250), both
  little-endian.
- `KTIMER_OP_EVENT` (2), descriptor-less: `payload[0..3]` is an event
  period in counts, little-endian; 0 stops the channel.  `arg0` >= 1
  arms it, `-KOS_EINVAL` without a subscription.  One subscriber at a
  time (`-KOS_EBUSY`), taken through `KOS_OP_SUBSCRIBE` (which the
  nucleus issues from `NOTIFY`, `docs/abi.md` section 6.7); the release
  stops the channel with it.  A stop (period 0) is valid whenever it
  comes, subscription or not: the unsubscribe and the release both drop
  the subscription before stopping the channel, so the stop cannot be
  behind the check for one being in place.  The driver claims `IRQ_CTC3`
  beside the tick's source, so both handlers live in this driver's
  window, and its channel-3 event timer is the first device-event
  source.

The boot segment resets channel 2, writes the shared IM2 vector base
(`00h`, so channel 2 vectors through page entry `04h`) and programs the
control word and time constant, from any state (warm reboot).  The
control word is composed in `hardware.h` from one named constant per
bit (`KOS_CTC_CTRL_*`), and the mode bit is the one that matters:
**timer mode** (D6 clear) counts the 10 MHz system clock through the 256
prescaler, while counter mode (D6 set) counts the channel's CLK/TRG
input - 7.3728 MHz on channel 2, from the board's 14.7456 MHz
oscillator's divider.  A control word with D6 set is therefore not a
slower or faster tick but a different clock: `E5h` with the same 250
counts interrupts at ~29.5 kHz (189x), and at 1,343 T-states per
delivery the CPU never leaves the handler.  The handler's whole body
increments the u32 tick count.

The reading runs with interrupts open - a syscall body gets the
caller's interrupt state, so the driver saves that state, opens for the
read and restores it (`timer_io.asm`) - so a rollover cannot put the
fresh counter beside the stale tick count.  The CTC holds one request,
so a masked stretch that spans two tick expiries loses one tick (any
mask longer than 6.4 ms can, depending on where it falls; one longer than
12.8 ms always does); the longest sections that mask are the IOC
driver's frame exchange and bulk receive, and a timed copy of a large
card file matches a stopwatch on the board.
`platform_irq_quiesce` stops channel 2 if the source is ever seen
unclaimed, and channel 3 too (a stopped event timer must not keep
interrupting); `platform_ctc_quiesce_all` stops all four physical
channels and is what `SYS_REBOOT` uses, because the CTC - not the reset
vector - remembers a channel that was programmed before the reboot.
Channel 0 stays free for the SIO0/A baud clock.  The SDK
wraps the clock as `kos/timer.h` (`docs/sdk.md` section 6.8) and the
event timer as `kos/event.h` (section 6.9).

**The event timer (channel 3).**  The clock above is its only time
reference.  The driver keeps an absolute deadline in clock counts and
adds the period at each one, so deadlines never drift.  Channel 3 covers
only the final stretch of each period: the tick handler reads the clock
exactly (the tick count was just incremented and the counter just
reloaded), and when the next deadline is within 256 counts it loads
channel 3 once with the counts left - reset, control word and constant,
so a load starts the channel from any state, the power-on one included.
Channel 3's handler stops the channel, reads the clock, posts every
deadline reached and adds the period; for a period shorter than 256
counts it loads the next stretch itself.  Its clock reading is one tick
low when a tick expired while it was being entered (the tick's own
handler has not run yet); channel 3 only expires at or after the
deadline, so a reading below the deadline is that stale one and is
lifted by a tick.  Nothing is counted on channel 3: a masked stretch
makes the post inside it late, never lost, doubled or early, and the next
deadline is exact again.  The posting (`event_post`) is the last thing
the handler does, so the delivery that follows it at the interrupt's exit
is as close to the deadline as the interrupt path allows.

The replay measures it (`tests/test_events.py`): in the run without the
emulated card peer the driver's own posts sit on the grid - median error
at or below 1 count, worst 16 - the handler's readings average out to an
interval sum within 2 counts over 49 periods, and the nucleus's delivery
costs 8,407 T-states per event, 4.2% of the CPU at 50 Hz: the frame
build and the hand-off (1,710), the trampoline's entry with its
argument-block snapshot (2,568) and the restore with the return gate and
the unwind (4,129).  The handler's own body is outside that figure.  The
card peer's emulated lane is paced per byte, so on that machine a masked
stretch can outlast a period and move a delivery's own reading without
the driver's deadlines moving; the peer harness therefore asserts only
what survives any masking - no lost or doubled event, every period
accounted for in `count`/`missed`, deadlines monotonic - and the
exact-deadline bounds are asserted in the replay without the peer and on
the board with `beat.exe` and a stopwatch.

The board check is the production store's `/system/beat.exe`
(`tests/beat/beat.c`): it subscribes at 500 ms, toggles a 440 Hz tone on
PSG0 at every delivery for ten seconds, prints `count/missed` once a
second, and silences the tone before it exits.  A steady
two-steps-per-second beat, a stopwatch on the ten seconds and a `missed`
of 0 are the check; the PSG is written through `/dev/sound` (one batched
`sound_write` per delivery) in conventional SN76489 byte order (the
card reverses the data bits, not the software).  Each line also carries the tick clock's own
reading (`clock <counts>`), and a second with no delivery at all is marked
` gap`: a stall then says which half stopped - a running clock under a
frozen count column is the delivery side, a frozen clock is the tick's.
`beat run <seconds>` is the same check for
another length, `beat reboot` warm-reboots after the fifth delivery while
still subscribed (channel 3 armed, channel 2 ticking; the machine must
come back to the shell), and `beat ls <dir> <seconds>` runs it while
enumerating a
directory - the card traffic the checklist asks for, which the monitor
cannot supply itself because its `X` run is synchronous, so the program
generates it.  The simulator cannot stand in for any of this: a
production replay never shows the program's console output, so the beat
is the board's to confirm.

## 10. SOUND (`drivers/sound/`)

The Afternoon Blend card: four SN76489 PSGs at `E0h-E3h` (input clock
3,579,545 Hz) and a DAC at `E4h`, published as `/dev/sound` and serving
the `SOUND` protocol (class 9, protocol type `SOUND`).  The driver owns
the card's lifecycle and a batched write.  It keeps no copy of the chips'
registers, because a program holding the device exclusively may write
them directly, so one reset routine recovers from any register state:

- **The reset** writes `9Fh BFh DFh FFh` to each PSG (attenuation 15 on
  all three tone channels and the noise generator) and the DAC's neutral
  code, `80h`.  It runs at the driver's init, at an exclusive acquire
  (`KOS_OP_ACQUIRE`), at `KSOUND_OP_RESET` and at the release of an
  exclusive hold (`KOS_OP_RELEASE`, `arg1` != 0).  An ordinary handle's
  release changes nothing.
- `KSOUND_OP_INFO` (0), descriptor-less: `payload[0..23]` is
  `struct ksound_info` - version 1, the `DIRECT_IO`, `BATCH_WRITE` and
  `DAC` capability flags, four PSGs at `E0h-E3h` (listed one by one), the
  DAC at `E4h`, the PSG clock in little-endian 3,579,545 Hz, and zero
  reserved bytes.
- `KSOUND_OP_WRITE` (1), descriptor IN, `arg0` = 0, `arg1` = the byte
  count: a sequence of `{target, value}` pairs, target 0-3 a PSG and 4
  the DAC, performed in order.  The count must be non-zero and even, and
  every target one the card has; otherwise the call is `-KOS_EINVAL`
  **before any write**, because the driver validates the whole sequence,
  chunk by chunk, before the first `OUT`.  It returns the bytes consumed.
  The pairs pass through a 64-byte `DRIVER_IO` buffer, 32 pairs per
  `copy_in`, and the write loop is assembly.
- `KSOUND_OP_RESET` (2), descriptor-less: the reset.
- `KOS_OP_ACQUIRE` takes the exclusive hold and resets the card; a second
  hold is `-KOS_EBUSY`.  WRITE and RESET are served on any handle.
- The exclusive holder reads the ports from `KSOUND_OP_INFO` and writes
  them directly (`sound_out` in the SDK); the hold's release resets the
  card, so the next program never inherits a sounding PSG.

The endpoint declares `"release": true` and no events or interrupt.  The
SN76489 bytes are conventional (a latch byte followed by its data byte);
the card reverses the data bits itself.  `sound_io.asm` is the only file
that touches `E0h-E4h`; `sound_proto.c` holds the dispatch, the INFO
record and the lifecycle.  Timing is the program's: a write happens when
it is sent, and the timer's events pace a music program.  The SDK
(`kos/sound.h`, `docs/sdk.md` section 6.11) splits a sequence longer than
one descriptor and checks the name's protocol type.

## 11. The `/dev` publication table

A driver manifest lists its publications: the name below `/dev`
(`"/serial/0"`), the protocol type, the serving endpoint and the object
identity. The packer resolves the endpoint and refuses a name that is the
root itself, a name too long for the 16-byte record, a duplicate, an
unknown class, or more rows than the nucleus table holds (12). Grouping
names (`/dev/input`, `/dev/serial`) are never published: the DEVICE
provider derives them from the names it installs. The loader checks each
row (name shape, type in range, a non-zero endpoint) and installs it.

From a program:

```c
unsigned char type;
int handle = bind("/dev/serial/0", 0, &type);   /* type = SERIAL */
call(handle, op, &req);
unbind(handle);
```

A grouping name enumerates but refuses to bind (`-KOS_EISDIR`). Names are
matched case-insensitively.

## 12. Interrupts

A driver claims a logical source through `irq_claim` in its boot bundle
and supplies a handler; the executive opens the driver's window around it
and owns the final `RETI` (architecture 4.5). The timer driver is the one
production driver-owned source: CTC channel 2's 156.25 Hz tick, claimed
by the `timer` image's init, with the handler in its hot segment. SIO is
polled, IOC and HID poll the same lanes, and the V9958 is register-driven.
The IPC acceptance image's `irqx` test driver covers the development
path against CTC channel 0. The production CTC code
(`kernel/irq/ctc.c`) is `platform_irq_quiesce()`, which also stops
channel 2, plus `platform_ctc_quiesce_all()` for the reboot path.

## 13. Placement

A driver's hot segment and `DRIVER_IO` data live in bank 6 `6000h-DFFFh`
(the `DRIVER_IO` part in `8000h-BFFFh`), its boot code and bulk data in
`0000h-5FFFh`. Nothing a driver owns enters common memory or bank 7.
`tools/mapcheck.py` fails the build if a driver symbol links into the
nucleus image, and `--production` rejects every acceptance-only symbol.

## 14. Device takeovers and state restoration

The Z80 cannot stop a program from writing to ports, so a driver's protocol
is not a fence: a program that wants to reprogram a device directly has to
ask for it, and the driver has to be told when the program is gone. Three
layers, in the order they catch a program:

1. **Release notification.** A driver whose manifest declares
   `"release": true` (`formats/include/mod.h`'s `MOD_EP_RELEASE`) is told
   when a handle on one of its objects goes away, explicitly with
   `unbind` or implicitly when the process exits: the nucleus calls the
   endpoint with `KOS_OP_RELEASE`, `arg0` = the object and `arg1` = 1 when
   the handle was the exclusive one. The result is ignored and an endpoint
   without the declaration costs nothing.
2. **The exclusive handle.** `KOS_BIND_EXCLUSIVE` asks for a takeover:
   the nucleus calls `KOS_OP_ACQUIRE` first, and the driver answers 0 or
   `-KOS_EBUSY` while another handle holds the object. **The contract:** a
   program writes a device's ports, or reprograms it beyond its protocol,
   only while it holds that device's exclusive handle — an ordinary bind
   is not permission to touch the hardware behind the driver. A device
   whose driver declares no release notification answers `-KOS_ENOTSUP`,
   because such a driver has nothing to restore.
3. **The console reset.** A program that wrote the ports with no handle at
   all left the driver no way to know. The kernel's boot launcher runs
   `KCONSOLE_OP_RESET` before every start of the boot program, so a
   take-over that nothing else caught cannot reach the next program's
   screen or input queue; and a warm reboot (`docs/abi.md` section 5.5)
   re-runs every driver's bring-up from any device state.

What each driver does with a release:

| device | held | released |
|---|---|---|
| `/dev/video` (`v9958`) | the console's output is dropped and counted; the driver touches no VDP register | the full display init runs again: mode registers, wait, palette, both font atlases, clear, cursor home |
| `/dev/console` (composition) | — (no exclusive form) | `KCONSOLE_OP_RESET`: terminal defaults, both font atlases, clear, cursor home, input queue drained |

`KVIDEO_OP_RESET` (3) runs the same display init at any time, and
`KCONSOLE_OP_RESET` (5) is the console's reset; the launcher uses the
ordinary console path, and the serial console resets only its own parser
because the video driver is not in its build.

Because no peripheral receives a hardware reset on a warm reboot, every
boot init programs its device from any state rather than from power-on
defaults, and a device a program reconfigured is rebuilt from the driver's
own constants.

## 15. Tests

| test | covers |
|---|---|
| `tests/test_driver_images.py`, `tests/test_mod.py` | the image format, the plan, endpoint numbering, dependencies, publications, the loader's bounds |
| `tests/test_ioc.py` | the IOC constants, the packed driver container and the driver's lane discipline |
| `tests/test_hid.py` | the HID contract, the packed driver container and the driver's fetch discipline |
| `tests/test_v9958.py` | the VIDEO contract, the font pipeline and the driver's reset and register discipline |
| `tests/test_console_acceptance.py` | the production image: load order, the IOC-FAT lane, and a scripted monitor session over the HID and V9958 lanes |
| `tests/test_console_serial.py` | the serial console build, and the missing-image boot failure |
| `tests/test_timer.py` | the TIMER contract and the production replay: the tick stream with the emulated CTC, interrupt cost, and IOC transfers under the timer |
| `tests/test_sound.py` | the SOUND contract and the production replay: INFO, batched writes, validation before any OUT, a sequence longer than one descriptor, the exclusive lifecycle, direct I/O and the resets |

The simulator covers control flow and register-level behaviour. Serial
timing, IOC lane timing, interrupt acknowledgement and real USB keyboards
are checked on the board (`docs/boot.md` section 8).
