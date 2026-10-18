# KonaCommander Protocol 1 (KCP1)

KCP1 is a stop-and-wait file browsing and transfer protocol for a 115200 8N1
serial link.  Multi-byte integers are little-endian.  Strings are UTF-8 on the
host; the target renders bytes directly through the KonaOS CP850 terminal, so
ASCII names are the portable subset.

## Framing

Each wire record is COBS-encoded and followed by one zero delimiter.  The
decoded record is:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `KCP1` |
| 4 | 1 | version, currently 1 |
| 5 | 1 | kind: 0 request, 1 response |
| 6 | 1 | command |
| 7 | 1 | sequence |
| 8 | 1 | status (zero in requests) |
| 9 | 1 | flags (zero in version 1) |
| 10 | 2 | payload length |
| 12 | N | payload, at most 768 bytes |
| 12+N | 2 | CRC-16-CCITT, little-endian |

CRC uses polynomial `0x1021`, initial value `0xffff`, no reflection, and no
final XOR.  It covers bytes 0 through 11+N.  The receiver discards malformed
records without acting on them.

The target permits one outstanding request.  It increments the sequence for a
new request and reuses it for retries.  A response must match kind, command,
and sequence.  The host remembers the exact last decoded request and encoded
response; an exact duplicate receives the cached response.  This makes a lost
OPEN response safe to retry.

## Common encodings

- `path` = `u8 length`, then that many bytes.  Length is 1..191 and the first
  byte is `/`.  `/` means the configured host root.
- `name` = `u8 length`, then 1..63 bytes; names never contain `/` or NUL.
- File sizes and offsets are `u32`.  Files larger than `0xffffffff` bytes are
  reported as `0xffffffff` and cannot be read past that offset.

## Commands

### 0x01 HELLO

Request payload is empty.  Response payload is:

```text
u16 server_max_payload
u8  display_name_length
u8[] display_name (0..63 bytes)
u8  capabilities (optional for compatibility; bit 0 permits CREATE)
```

A response without the final capability byte is accepted as a read-only
phase-6 server.  Current servers always include it.

### 0x02 LIST

Request:

```text
path directory
u32  cursor       (zero starts the sorted list)
u8   wanted       (1..32)
```

Response:

```text
u32 next_cursor   (zero means end)
u8  count
repeat count times:
    u8 flags      (bit 0 directory; all other bits zero)
    u32 size      (zero for directories)
    name
```

The host sorts directories before files and then by Unicode case-folded name.
The cursor is a sorted entry index.  Entries whose encoded name exceeds 63
bytes are omitted, which keeps every returned item representable by the
target.

### 0x03 OPEN

Request is one `path` naming a regular file.  Response:

```text
u8  handle        (1..8)
u32 size
```

### 0x04 READ

Request:

```text
u8  handle
u32 offset
u16 wanted        (1..768)
```

Response:

```text
u32 offset        (echoed)
u16 count
u8  data[count]
```

An offset at or past EOF succeeds with count zero.

### 0x05 CLOSE

Request is one handle byte.  The response payload is empty.  Closing an
already closed handle succeeds, which simplifies cleanup after retries.

### 0x06 PING

The response echoes the request payload (0..32 bytes).

### 0x07 CREATE

Request is one destination `path`.  The command is denied unless the host was
started with `--write`.  The host creates a unique temporary file in the
destination directory and returns:

```text
u8 handle (1..8)
```

The existing destination, if any, is not changed by CREATE.  A write handle
must be finished with COMMIT or discarded with ABORT/CLOSE.

### 0x08 WRITE

Request:

```text
u8  handle
u32 offset
u16 count        (1..761)
u8  data[count]
```

Response echoes `offset` and `count`.  Writes must be sequential: offset must
equal the temporary file's current length.  The stop-and-wait duplicate cache
makes retry of the most recent WRITE idempotent.

### 0x09 COMMIT

Request is one write-handle byte.  The host flushes and synchronizes the
temporary file, closes it, then atomically replaces the destination using the
host filesystem's same-directory rename operation.  The response is empty.

### 0x0A ABORT

Request is one handle byte.  The host closes the handle and removes its
temporary file.  ABORT is idempotent and has an empty response.  CLOSE also
aborts a write handle, making generic cleanup safe.

## Status values

| Value | Name | Meaning |
|---:|---|---|
| 0 | OK | success |
| 1 | BAD_REQUEST | malformed payload or unsupported flags |
| 2 | NOT_FOUND | path does not exist |
| 3 | NOT_DIRECTORY | LIST target is not a directory |
| 4 | NOT_FILE | OPEN target is not a regular file |
| 5 | DENIED | path resolves outside the configured root or access denied |
| 6 | BAD_HANDLE | READ handle is not open |
| 7 | NO_HANDLES | all eight host file slots are occupied |
| 8 | IO_ERROR | host I/O failed |
| 9 | UNSUPPORTED | command or version is unsupported |
| 10 | TOO_LARGE | request exceeds a protocol limit |

Errors have an optional printable UTF-8 diagnostic payload of at most 96
bytes.  It is for the status bar only and must not be parsed programmatically.

