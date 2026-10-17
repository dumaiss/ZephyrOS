# KonaOS Files

Files and directories: the protocols programs use, and the filesystem
driver behind `/fs`. The rules are in `docs/architecture.md` sections 4.10
and 6.5; the namespace implementation is `docs/vfs.md`.

## 1. Where files live

| root | provider | writable | backing |
|---|---|---|---|
| `/fs` | the filesystem driver the build selected (IOC-FAT) | yes | the SD card, through the MCU's FatFs |
| `/system` | ROMFS, in the nucleus | no (`KOS_EROFS`) | the ROM recovery store |

`/system` is available before any driver and with no card. `/fs` without a
card answers `KOS_ENODEV`. There are no drive letters: the namespace is one
tree.

## 2. Programs' view

The SDK (`kos/fs.h`) gives the familiar words:

| function | how |
|---|---|
| `open(path, flags)` | `bind`, then check the type is `FILE` |
| `read`, `write`, `seek`, `sync` | `FILE` protocol calls |
| `close(file)` | `sync`, then `unbind` |
| `stat`, `mkdir`, `rmdir`, `unlink`, `rename` | `FS` protocol calls on the provider root |
| `list(path, ...)` | `ns_enum` |
| `chdir`, `getcwd` | syscalls |

Open flags (`abi/kos/fs.h`): `KFS_O_READ` (`01h`), `KFS_O_WRITE` (`02h`),
`KFS_O_CREATE` (`04h`), `KFS_O_EXCL` (`08h`), `KFS_O_TRUNC` (`10h`). Seek
origins: `KFS_SEEK_SET`, `KFS_SEEK_CUR`, `KFS_SEEK_END`.

Rules:

- I/O is byte-oriented. A zero-byte read is end of file; short reads are
  valid. Sizes and positions are 32-bit; one transfer is up to 16-bit.
- A write advances the position only after the driver has confirmed it.
- `sync` flushes a file and keeps it open; `close` flushes and releases it.
  There is no filesystem-wide sync.
- `rename` within one provider uses the provider's rename. Across providers
  it returns `KOS_EXDEV`; the system never turns a rename into a copy.
- Truncation is `KFS_O_TRUNC` at open; there is no truncate-to-size and no
  `statfs`.

## 3. The FILE protocol (type `FILE`)

On a bound file. `payload[0]` of every operation is the instance identity,
which the nucleus stamps from the handle, so a driver finds its per-file
state without a table of handles.

| op | number | frame | result |
|---|---|---|---|
| `KFILE_OP_READ` | 0 | desc out, offset, length | bytes read (0 at end of file); advances the position |
| `KFILE_OP_WRITE` | 1 | desc in, offset, length | bytes written; `KOS_EROFS` on a read-only provider |
| `KFILE_OP_SEEK` | 2 | `arg0` = origin, `payload[1..4]` = signed 32-bit offset | 0; the reply's `payload[1..4]` is the new position |
| `KFILE_OP_META` | 3 | — | 0; the reply payload is a `struct kfs_stat` |
| `KFILE_OP_SYNC` | 4 | — | 0 after the flush |

## 4. The filesystem-namespace protocol (type `FS`)

`bind("/fs", 0)` or `bind("/system", 0)` returns a namespace handle for name
operations. The path below the root (`/GAMES` for `/fs/GAMES`) goes in the
descriptor. The numbers start at 8 so one dispatch can serve both
protocols.

| op | number | descriptor | result |
|---|---|---|---|
| `KFS_OP_MKDIR` | 8 | in: the path | 0 |
| `KFS_OP_RMDIR` | 9 | in: the path (must be empty) | 0 |
| `KFS_OP_UNLINK` | 10 | in: the file | 0 |
| `KFS_OP_RENAME` | 11 | in: the source path, NUL, the target path | 0 |
| `KFS_OP_STAT` | 12 | in: the path | 0; the reply payload is a `struct kfs_stat` |

Between the nucleus `/fs` provider and the driver, three more operations
carry the path through the fixed-core workspace instead of a descriptor:

| op | number | frame | role |
|---|---|---|---|
| `KFS_OP_OPEN` | 13 | `payload[0..1]` path address, `payload[2..3]` flags; reply `payload[0]` = instance | the provider's `bind` |
| `KFS_OP_CLOSE` | 14 | `payload[0]` = instance | the provider's `unbind` |
| `KFS_OP_ENUM` | 15 | desc out: the caller's entries; `payload[0..1]` path, `payload[2..3]` cookie, `payload[4..5]` byte count | `ns_enum` below `/fs`; replies the next cookie |

## 5. Metadata

```c
struct kfs_stat {           /* 16 bytes */
    uint8_t  type;          /* KFS_TYPE_FILE (1) or KFS_TYPE_DIR (2) */
    uint8_t  attributes;    /* KFS_ATTR_READONLY 01h, HIDDEN 02h, SYSTEM 04h */
    uint16_t reserved;
    uint32_t size;          /* 0 for a directory */
    uint32_t ctime;         /* KFS_TIME_UNKNOWN (0): the machine has no clock */
    uint32_t mtime;
};
```

There is no archive bit, and no inode numbers, clusters, tokens, owners or
modes. Times are 32-bit so a clock can be added without changing the
layout. Enumeration entries are the namespace's 28-byte records
(`docs/abi.md` section 6.5), with a 24-byte name.

## 6. IOC-FAT (`drivers/iocfat/`)

The deployed MCU runs FatFs and speaks the FS2 command set over IOC.
IOC-FAT turns the FILE and FS protocols into FS2 commands. It owns path
meaning, creation, mutation, enumeration and the backend state below
`/fs`; the nucleus provider only routes.

**Limits.**

- Two open files and one open directory (the firmware's slot counts). More
  is `KOS_EMFILE`.
- **Names are 8.3.** Each path component is packed into an 11-byte
  upper-case FCB name; a component FatFs cannot represent (a base name over
  8 characters, an extension over 3, a forbidden character) is refused with
  `KOS_EINVAL` before any IOC traffic. The native ABI allows 63-byte names;
  this driver does not.
- Paths are resolved fresh for each operation: `ROOT`, then one `PUSH` per
  directory component.
- The enumeration cookie is the entry index. A batch that fills before the
  end keeps the directory open, and the next batch for the same path at
  that cookie reads on; one that reaches the end remembers it, so the
  caller's last page costs nothing. Any other batch reopens the directory
  and skips to its cookie.
- The entries of the last batch are remembered (name, type, size), and a
  `stat` of one of them is answered from that memo, once per entry. A
  listing that stats what it lists (the shell's `ls`) touches the card
  once per entry.
- Any operation other than a batch or a `stat` closes the kept directory
  and forgets the memo, and so does a new media generation.

**FS2 commands** (private to the driver): `CAPS` 30h, `GENERATION` 31h,
`RESET` 32h, `ROOT` 33h, `PUSH` 34h, `OPEN_RO` 35h, `READ` 36h, `CLOSE` 37h,
`OPENDIR`
38h, `READDIR` 39h, `CLOSEDIR` 3Ah, `STAT` 3Bh, `OPEN_RW` 3Dh, `WRITE` 3Eh,
`SYNC` 3Fh, `TRUNCATE` 40h, `UNLINK` 41h, `RENAME` 42h, `MKDIR` 43h,
`RMDIR` 44h, and `XFER_STATUS` 06h for bulk DONE. FS2 tokens never leave the
driver: a bound file's identity is its slot index plus one.

**Bring-up resets the controller's contexts.** The MCU owns its file and
directory slots, and they outlive the Z80 side: a warm reboot (`docs/abi.md`
section 5.5) leaves the controller holding every slot a program opened when
the machine restarted. The driver's init therefore sends `RESET` — the same
command a media change answers, which closes every context, advances the
generation and resets the resolver — before it reads `CAPS`, so a boot
starts from the state `CAPS` describes rather than from whatever the
previous boot left open.

**Status mapping.**

| FS2 status | error |
|---|---|
| `NOT_FOUND` 40h | `KOS_ENOENT` |
| `EXISTS` 42h | `KOS_EEXIST` |
| `BAD_NAME` 43h, `RANGE` 4Ah | `KOS_EINVAL` |
| `READ_ONLY` 44h | `KOS_EROFS` |
| `NO_SPACE` 45h | `KOS_ENOSPC` |
| `NOT_DIR` 46h | `KOS_ENOTDIR` |
| `IS_DIR` 47h | `KOS_EISDIR` |
| `NO_HANDLE` 48h | `KOS_EMFILE` at open, `KOS_ESTALE` otherwise |
| `STALE` 49h | `KOS_ESTALE` |
| `NO_MEDIA` 4Bh | `KOS_ENODEV` |
| `IO` 4Eh, transport failures, anything else | `KOS_EIO` |

**The file-data cache.** One 512-byte slot in the driver's `DRIVER_IO`
area, keyed by the FS2 file, its generation and an aligned file offset:

- a read hit is served locally; a miss reads the aligned block over FS2
  and fills the slot; a read across a block boundary is split;
- writes are write-through: FS2 write, then the confirmed DONE, then the
  slot is updated or invalidated. A failed or unconfirmed write changes no
  cached byte;
- a confirmed write, a truncating open, `unlink` or `rename` also
  invalidates the other open file's data (FS2 serves at most two files);
- a generation change or a `STALE` reply invalidates everything.

Nothing counts as written because it reached the bulk lane. The VFS keeps
no cache of its own, and ROMFS needs none.

**No media.** See `docs/devices.md` section 8: the driver still publishes,
and every operation answers `KOS_ENODEV`. A card counts as present only once
the root has been opened and read.

## 7. Other filesystems

The `/fs` provider forwards to whichever filesystem driver the build
selects, so another filesystem (a Z80-side FAT or a native format on raw
storage) is a new driver behind the same two protocols. It would define the
block protocol it needs as an IPC protocol, with any block cache inside the
filesystem driver, not in IOC and not in the VFS. Programs, the shell and
the namespace would not change.

## 8. Tests

- `tests/test_iocfat.py`: the driver's names, limits, status mapping and
  cache rules on the host.
- `tests/test_fs_selftest_sim.py`: the `fs-selftest` image, every file
  operation against the simulator's FS2 peer serving a card, the card-absent
  run, handle release and nested exec.
- `tests/test_hwtest_sim.py`: the production exerciser's file stage, with a
  card and without one.
