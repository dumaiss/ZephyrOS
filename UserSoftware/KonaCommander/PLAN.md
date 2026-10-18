# KonaCommanderKos implementation plan

This file is the hand-off contract for the project.  An interrupted agent
should be able to resume by reading this file, `PROTOCOL.md`, and the latest
test output.  Phases 1-6 established browsing and viewing; the implemented
phase-7/8 subset adds confirmed, bidirectional single-file copy.

## Current implementation state

Phases 1-6 and bidirectional single-file copy are implemented as of
2026-10-17.  `make test` passes the portable C codec vectors, Python
protocol/service/security/write tests, stdio integration, and EXE layout
checks.  `make` builds and validates
`build/user/konacmd.exe` from SDK archive SHA-256
`c092ea1a9c6f2fc936cca30a3737bf4e07ebcd3790b500665a259007a0e085eb`.
The current EXE SHA-256 is
`0c7b1bfb661c086a75cf199ab5999fd8ef566b953beaec5e3c212a9404c7cdcd`;
its primary image plus BSS ends at `999Eh` and its stack is `DC00h-DFFFh`.

No physical-board copy session has been performed in this environment.  The
next action is the README hardware smoke test.
Startup is intentionally offline-capable: an absent PC server produces an
empty live UI, and `r` performs a later connection or reconnection.

## Product boundary

KonaCommander is a native KonaOS program (`konacmd.exe`) plus a Python host
companion (`host/kcommander_host.py`).  The Kona program owns the V9958/HID
console and talks to the PC through `/dev/serial/0`; the host companion owns a
serial device and exposes exactly one configured directory as virtual `/`.

The program has a local KonaOS `/fs` pane and a PC directory pane, directory
traversal, selection and paging, a full-screen text/hex viewer, and F5 copy of
one selected file to the opposite pane.  Copy always asks before replacing a
name.  It does not copy directories, delete, rename interactively, execute,
or create directories.

The target requires the normal V9958/HID console build.  A serial-console
KonaOS build cannot be used because `/dev/console` and `/dev/serial/0` are the
same physical SIO0/B byte stream in that configuration.

## Non-negotiable invariants

- Use only the public SDK from
  `../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz`; never call kernel entry
  points or hardware ports directly.
- Bind `/dev/serial/0`, verify `KOS_TYPE_SERIAL`, and use batched
  `KSIO_OP_WRITE`/`KSIO_OP_READ` calls.  No byte-at-a-time serial IPC.
- Pace PC-to-Kona bytes (4 ms by default).  The polled SIO driver drains a
  burst but does not wait for its next byte, and application IPC/timer
  re-entry is slower than an unpaced 115200-baud stream.  Client receive uses
  a one-second inactivity timeout plus a six-second absolute frame bound.
- All wire messages are COBS-delimited and CRC-16-CCITT protected.  A parser
  must reject malformed length, version, CRC, type, command, and sequence.
- There is at most one request in flight.  Retries reuse the same sequence.
  The host caches the last complete response so duplicate `OPEN` does not
  leak handles.
- The host resolves every requested path below its configured root after
  symlink resolution.  Absolute host paths, `..` escapes, and symlinks that
  escape are denied.
- Buffers are fixed-size on the Z80; no allocation and no recursion.
- Every decoded length is checked before indexing or copying.
- The host remains read-only unless explicitly started with `--write`.  Host
  uploads use CREATE/WRITE/COMMIT and same-directory atomic replacement;
  ABORT and host shutdown remove incomplete temporary files.
- PC-to-KonaOS copy writes and syncs `KCTMPn.$$$` first.  Only then is an old
  destination removed and the temporary file renamed.  KonaOS lacks an
  atomic replace operation, so a failed final rename may leave the temporary
  file for recovery.
- Do not assume a directory fits in RAM.  LIST is cursor-paged and each pane
  keeps only one page.
- Do not assume a file fits in RAM.  The viewer uses positioned READ requests.

## Wire protocol summary

The exact byte layout and command payloads live in `PROTOCOL.md`.  The stable
outline is:

```text
wire       COBS(decoded-frame) 00
frame      "KCP1", version, kind, command, sequence, status, flags,
           payload-length-le16, payload, crc16-le
discipline one request, one response; target retries after a bounded timeout
```

Commands are `HELLO`, `LIST`, `OPEN`, `READ`, `CLOSE`, `PING`, `CREATE`,
`WRITE`, `COMMIT`, and `ABORT`.
Payloads use explicit little-endian integers and length-prefixed UTF-8 path or
name bytes.  Target-visible names are limited to 63 bytes and paths to 191
bytes.  File offsets and sizes are unsigned 32-bit values.  The data payload
limit is 768 bytes, chosen to keep target RAM modest while making serial calls
coarse-grained.

## Source layout

```text
Makefile                    SDK bootstrap, target build, host tests
PLAN.md                     this hand-off document
PROTOCOL.md                 normative protocol specification
README.md                   operator/build/key documentation
src/kc_protocol.[ch]        portable CRC, COBS and frame validation
src/kc_serial.[ch]          KonaOS SERIAL calls and retrying transaction layer
src/kc_remote.[ch]          typed browse/read/write protocol client
src/kc_ui.[ch]              85x26 renderer, key decoder, panes and viewer
src/main.c                   startup, cleanup and top-level event loop
host/kcommander_host.py      sandboxed host service and serial CLI
tests/test_host.py           protocol/server/security/write tests
tests/test_protocol.c        portable C codec tests compiled by host cc
tests/include/...            only if a narrow host compatibility shim is needed
```

Keep protocol encoding in one portable C module so host-side tests can verify
the exact code linked into `konacmd.exe`.  Keep KonaOS calls out of that module.

## Phase gates

### Phase 1 - specification and skeleton

Deliver this plan first, then `PROTOCOL.md`, directory layout, SDK-bootstrap
Makefile, common constants, and a README stub.  Gate: another developer can
identify every component, limit, and phase without reading chat history.

### Phase 2 - tested protocol codec

Implement COBS encode/decode, CRC-16-CCITT, frame encode/decode, LE helpers,
and strict validation in portable C.  Add C and Python golden-vector tests.
Gate: corrupted CRC, truncation, oversize payload, embedded zero bytes, and a
maximum-size round trip are tested on the host.

### Phase 3 - read-only PC companion

Implement the protocol service independently of pyserial, then a serial CLI.
LIST must sort directories first and compare names case-insensitively; pages
use a numeric sorted-list cursor.  OPEN has a bounded handle table, READ is
positioned and bounded, CLOSE is idempotent, and repeated identical requests
return a cached response.  Gate: temporary-directory tests prove traversal,
paging, file reads, duplicate requests, bad handles, and root/symlink escape
rejection.  `--stdio` is available for plumbing tests; normal mode uses
pyserial at 115200 8N1.

### Phase 4 - KonaOS serial and remote client

Implement batched SERIAL descriptors, receive accumulation to the zero frame
delimiter, bounded timer-based timeout, retry with the same sequence, response
validation, and typed remote operations.  Bind and type-check both serial and
timer devices.  Gate: target links with the current packaged SDK and the EXE
packer validates it; host tests cover payload parsing shared with the target.

### Phase 5 - Commander TUI and navigation

Implement a stable 85x26 full-screen layout using the console driver's ANSI
subset: title bar, one 42-column KonaOS pane, one 42-column PC pane,
reverse-video active selection, status line, and key bar.  Each pane owns a
path, page cursor, next cursor, selection, and one page of entries.  Support
Up/Down, PageUp/PageDown, Tab, Enter, Backspace, refresh, help, and quit.
Enter on a directory descends; Backspace ascends toward the provider root.
Ordinary selection movement updates only two cells and the detail row; it must
not repaint the screen.  Gate: all screen writes are bounded, paths are
bounded, an empty/error directory remains navigable, and both panes update
independently.

### Phase 6 - useful file viewer and hardening

Implement F3/`v` full-screen viewing, automatically selecting text unless a
sample contains NUL or excessive controls.  F4/`x` toggles text/hex.  Reads
are positioned and page-sized, so large files work.  PageUp/PageDown and
arrow keys move, status shows path/mode/offset/size, and Esc/F10/`q` returns
without losing pane state.  Close the local or remote handle on every
exit/error path.

Finish operator errors (server absent, timeout, CRC/protocol failure, denied
path, stale handle), reconnect/refresh, clean terminal restoration, host
shutdown cleanup, documentation, and the complete test/build pass.  Gate:
`make test` passes, `make` produces a validated KonaOS EXE using the packaged
SDK, and README contains an explicit hardware smoke-test procedure.

### Phase 7/8 copy subset - implemented

The left pane is KonaOS `/fs`, the right pane is the PC export, and F5 copies
the selected regular file in either direction after confirmation.  Transfers
are 700-byte streamed chunks with progress.  PC writes require `--write` and
use atomic temporary-file replacement.  KonaOS destinations use a synced
temporary file before best-effort replacement.  Automated tests cover host
write opt-in, commit, abort, duplicate WRITE, and cleanup.

### Still later

Directory copy, mkdir, interactive rename/delete, search, filters, bookmarks,
transfer resume, and rate/ETA display remain out of scope.

## Resume checklist

1. Run `git status --short` in `Zephyr-80`; do not alter sibling untracked
   projects or user changes.
2. Read this file and `PROTOCOL.md` completely.
3. Run `make test` in this directory, then `make`.
4. Inspect `build/user/konacmd/konacmd.report.json` for image/BSS/stack bounds.
5. Continue from the first phase gate that is not met; update this file only
   when scope or a durable invariant actually changes.
6. Before hand-off, remove Python cache files and report that hardware serial
   execution is still unverified unless it was really performed.
