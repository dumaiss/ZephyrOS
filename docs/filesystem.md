# ZephyrOS M9 Filesystem Architecture Proposal

**Status:** Draft for review  
**Scope:** M9 native filesystem architecture  
**Target:** Zephyr-80  
**Purpose:** Define the native ZephyrOS filesystem framework before implementation. This document intentionally excludes CP/M filesystem semantics.

---

## 1. Goals

M9 gives ZephyrOS a real native filesystem framework rather than hard-wiring the OS to the current MCU FAT implementation.

The architecture must support:

- one native hierarchical namespace rooted at `/`;
- no native drive letters;
- multiple filesystem implementations;
- static boot-time mount configuration;
- ROMFS as a normal filesystem implementation;
- the current MCU/FatFs service as one filesystem provider;
- raw SD access through a generic block-device abstraction;
- future filesystems implemented directly on the Z80, such as VFAT or a Fuzix filesystem;
- path-, handle-, and byte-oriented native APIs;
- a local Z80-side file-data cache to avoid repeated transport traffic;
- a clean boundary for later personalities such as CP/M.

The central rules are:

> **VFS owns the namespace. Filesystems provide trees attached to paths in that namespace.**

and:

> **IOC is transport only. Filesystem and block semantics live above it.**

---

## 2. High-level layering

The filesystem stack is divided into independent concepts:

```text
native applications
        |
        v
+-----------------------+
|         VFS           |
| namespace / paths     |
| mounts / CWD / fd     |
| file-data cache       |
+-----------------------+
        |
        +---------------------+----------------------+
        |                     |                      |
        v                     v                      v
   ROMFS filesystem      IOC-FAT filesystem      future FS
                              |                      |
                              |                      v
                              |                block layer
                              |                      |
                              |                  ZDEV_BLOCK
                              |                      |
                              |                 SD-over-IOC
                              |                      |
                              +----------+-----------+
                                         |
                                         v
                                      ZDEV_IOC
                                         |
                                         v
                                        MCU
                                         |
                                         v
                                         SD
```

Ownership is deliberately separated:

- **VFS** owns the global namespace and process-visible filesystem semantics.
- **Filesystem providers** own filesystem-specific behavior.
- **Block devices** expose raw sector/block storage.
- **IOC** owns only the Z80-to-MCU transport.

The MCU currently provides both raw SD primitives and a FatFs-based filesystem service. ZephyrOS exposes both without making either one the architectural foundation of the other.

---

## 3. Native namespace

ZephyrOS has exactly one native namespace:

```text
/
```

There are no native equivalents of:

```text
A:
B:
C:
USER 0
USER 15
```

Those belong, if needed, to later compatibility personalities.

The initial system layout is:

```text
/           -> writable SD FAT filesystem
/SYSTEM     -> internal ROMFS
```

Typical visible contents might be:

```text
/
|-- GAMES/
|-- TOOLS/
|-- DOCS/
`-- SYSTEM/
    |-- monitor.zex
    |-- shell.zex
    |-- sio.zmod
    |-- ioc.zmod
    `-- ...
```

Future devices may be attached at additional root-level mountpoints:

```text
/CF
/SD2
/EXTERNAL
/BACKUP
```

No drive-selection state exists in native ZephyrOS.

---

## 4. VFS owns `/`

The root namespace exists independently of any mounted filesystem.

This matters because `/SYSTEM` must remain accessible even if the SD card is absent, corrupt, or unavailable.

Conceptually:

```text
VFS namespace
/
|-- SYSTEM     -> ROMFS
|-- CF         -> future external filesystem
`-- default tree supplied by filesystem attached at /
```

The filesystem attached at `/` is the normal writable filesystem, but it does **not** own the namespace.

If the root filesystem is unavailable, the namespace can still contain:

```text
/
`-- SYSTEM/
```

This preserves recovery behavior.

---

## 5. Static mount configuration

M9 does not provide runtime `mount` or `umount` commands.

Mount topology is defined by build-time JSON configuration and established during boot.

The configuration file should use the same manifest-like style already used elsewhere in the build.

Suggested file:

```text
config/mounts.json
```

Suggested schema:

```json
{
  "format": "vfs-mounts-v1",
  "mounts": [
    {
      "path": "/SYSTEM",
      "filesystem": "romfs",
      "source": "recovery",
      "system": true
    },
    {
      "path": "/",
      "filesystem": "iocfat",
      "source": "ioc.0",
      "required": false
    }
  ]
}
```

Exactly one configured mount must be marked:

```text
system = true
```

The system filesystem is instantiated during early VFS bootstrap, before any loadable modules are required. Its provider must therefore be statically available and must have no dependency on loadable modules or dynamically registered devices.

Field semantics:

```text
path
    Absolute VFS mountpoint.

filesystem
    Name of a filesystem provider registered with VFS.

source
    Opaque provider-specific source identifier.
    VFS stores/passes it but does not interpret it.

system
    true:
        this is the unique bootstrap filesystem.
        it is instantiated immediately after VFS core initialization.
        its provider must already be available in the kernel image.
        it may not depend on loadable modules or dynamically registered devices.
        failure to instantiate it is fatal.

required
    true:
        for non-system mounts, failure to instantiate the mount is a
        boot/configuration failure.

    false:
        for non-system mounts, mount failure is tolerated and the remaining
        namespace stays usable.
```

`system: true` is stronger than `required: true`; the system entry does not need a separate `required` field.

Future example:

```json
{
  "format": "vfs-mounts-v1",
  "mounts": [
    {
      "path": "/SYSTEM",
      "filesystem": "romfs",
      "source": "recovery",
      "system": true
    },
    {
      "path": "/",
      "filesystem": "iocfat",
      "source": "ioc.0",
      "required": false
    },
    {
      "path": "/CF",
      "filesystem": "vfat",
      "source": "cf.0",
      "required": false
    },
    {
      "path": "/SD2",
      "filesystem": "fuzixfs",
      "source": "sd.1",
      "required": false
    }
  ]
}
```

The `source` value is provider-specific:

```text
romfs:
    "recovery"
        -> built-in recovery ROM source

iocfat:
    "ioc.0"
        -> MCU filesystem service

vfat:
    "cf.0"
        -> ZDEV_BLOCK instance

fuzixfs:
    "sd.1"
        -> another ZDEV_BLOCK instance
```

VFS does not interpret provider-specific source strings.

### 5.1 Manifest validation

The host-side build tool must reject at least:

```text
unknown format/version
missing required fields
empty filesystem/source names
zero system filesystems
more than one system filesystem
system filesystem marked as depending on a loadable provider
relative mount paths
paths longer than ZOS_PATH_MAX
'.' or '..' in configured mountpoints
duplicate mountpoints
case-only duplicate mountpoints
duplicate '/'
unsupported nested mountpoints in M9
```

Because native VFS pathname matching is case-insensitive, these collide:

```text
/SYSTEM
/system
/System
```

Configured mount paths should be emitted in a canonical spelling, preferably uppercase for readability.

---

## 6. Initial M9 mount restrictions

M9 intentionally keeps mount topology simple.

Configured mountpoints are limited to:

```text
/
or
/<single-root-component>
```

Examples:

```text
/SYSTEM
/CF
/SD2
```

Arbitrary nested mountpoints may be added later if a real use case appears.

This is a simplifying implementation restriction, not a fundamental VFS limitation.

---

## 7. Root directory enumeration

Because VFS owns the namespace, `readdir("/")` is a VFS operation.

The visible root directory is the merge of:

1. entries from the filesystem attached at `/`; and
2. configured root-level mountpoints.

For example, if the SD root contains:

```text
GAMES
TOOLS
README.TXT
```

and VFS has:

```text
/SYSTEM
/CF
```

then `ls /` presents:

```text
CF/
GAMES/
SYSTEM/
TOOLS/
README.TXT
```

If the root filesystem also contains a real entry named `SYSTEM`, the VFS mountpoint wins and shadows that backing entry while mounted.

This rule makes root resolution deterministic.

---

## 8. Path resolution

VFS owns:

- absolute versus relative paths;
- current working directory;
- normalization;
- case-insensitive comparison;
- mountpoint selection;
- filesystem-relative path construction.

Native paths use `/`.

M9 supports:

```text
/
/SYSTEM
/GAMES/FOO.ZEX
.
..
repeated slash normalization
relative paths
absolute paths
```

Example:

```text
cwd = /GAMES

open("SAVE.DAT")
    -> /GAMES/SAVE.DAT

open("../TOOLS/EDIT.ZEX")
    -> /TOOLS/EDIT.ZEX
```

Traversal above `/` remains at `/`.

### 8.1 Case-insensitive namespace

Native VFS pathname comparison is ASCII case-insensitive.

Thus:

```text
/system/monitor.zex
/SYSTEM/MONITOR.ZEX
/System/Monitor.Zex
```

refer to the same namespace object.

Mountpoint comparison follows the same rule.

The comparison rule is intentionally simple:

```text
A-Z == a-z
all other byte values compare exactly
```

ZephyrOS does not attempt Unicode case folding.

### 8.2 Mount selection

VFS resolves the owning mount before calling a filesystem implementation.

Example:

```text
/SYSTEM/monitor.zex

VFS mount      = /SYSTEM
FS-relative    = /monitor.zex
filesystem     = ROMFS
```

while:

```text
/GAMES/DOOM.ZEX

VFS mount      = /
FS-relative    = /GAMES/DOOM.ZEX
filesystem     = IOC-FAT
```

A filesystem implementation never needs to know that its own root appears as `/SYSTEM`, `/CF`, or `/`.

---

## 9. Path and component limits

M9 pins:

```text
ZOS_PATH_MAX = 128
ZFS_NAME_MAX = 64
```

`ZOS_PATH_MAX` includes the terminating NUL for user-facing C wrappers.

The syscall ABI should still use explicit pointer/length fields rather than require unbounded kernel scans for NUL.

`ZFS_NAME_MAX = 64` deliberately avoids baking FAT 8.3 restrictions into the native ABI.

The current IOC-FAT provider may return much shorter names if that is all the deployed MCU firmware supports.

---

## 10. VFS responsibilities

A new kernel subsystem should own the native filesystem namespace:

```text
kernel/vfs/
```

Its responsibilities include:

```text
global namespace
configured mount table
path normalization
case-insensitive mount/path comparison
mountpoint resolution
current working directory
process-visible file descriptors
directory handles
file offsets
file-data cache
filesystem syscall dispatch
user-buffer validation and copying
process cleanup
cross-filesystem policy
```

VFS must not contain:

```text
FAT directory parsing
FS2 command numbers
IOC READY/DONE handling
ROMFS on-ROM layout
SD-sector commands
CP/M FCB logic
```

Those belong to lower or later layers.

---

## 11. Filesystem-provider registration

Filesystem providers register directly with VFS.

They are not represented as hardware devices merely because some providers are loadable ZMODs.

Conceptually:

```c
int vfs_register_filesystem(const struct zfs_provider *provider);
```

with a provider descriptor semantically equivalent to:

```c
struct zfs_provider {
    const char *name;
    const struct zfs_ops *ops;
    void *provider_state;
};
```

This creates a distinct separation:

```text
device registry
    SIO
    IOC
    HID
    VIDEO
    BLOCK

VFS filesystem-provider registry
    ROMFS
    IOC-FAT
    future VFAT
    future Fuzix filesystem
```

A filesystem provider registers by filesystem type/provider name, not by pathname.

The build-time mount configuration selects a registered provider and attaches an instance to a namespace path.

---

## 12. Filesystem-provider interface

Every filesystem implementation presents the same internal interface to VFS.

The exact ABI may be finalized during implementation, but it should be semantically equivalent to:

```c
struct zfs_ops {
    int (*mount)(void *provider_state,
                 const char *source,
                 void **fs_state);

    int (*unmount)(void *fs_state);

    int (*lookup)(void *fs_state,
                  void *parent,
                  const char *name,
                  void **node);

    int (*stat)(void *fs_state,
                void *node,
                struct zfs_stat *out);

    int (*open)(void *fs_state,
                void *node,
                uint16_t flags,
                void **file);

    int (*close)(void *fs_state,
                 void *file);

    int (*read)(void *fs_state,
                void *file,
                uint32_t offset,
                void *buffer,
                uint16_t length);

    int (*write)(void *fs_state,
                 void *file,
                 uint32_t offset,
                 const void *buffer,
                 uint16_t length);

    int (*sync)(void *fs_state,
                void *file);

    int (*truncate)(void *fs_state,
                    void *file,
                    uint32_t size);

    int (*opendir)(void *fs_state,
                   void *node,
                   void **dir);

    int (*readdir)(void *fs_state,
                   void *dir,
                   struct zfs_dirent *out);

    int (*closedir)(void *fs_state,
                    void *dir);

    int (*mkdir)(...);
    int (*rmdir)(...);
    int (*unlink)(...);
    int (*rename)(...);

    int (*syncfs)(void *fs_state);

    int (*statfs)(void *fs_state,
                  struct zfs_statfs *out);
};
```

Opaque filesystem handles remain owned by the provider.

Applications never see:

```text
ROMFS entry indices
FatFs FIL objects
FS2 slot/cookie tokens
FAT cluster numbers
future Fuzix inode structures
```

---

## 13. ROMFS becomes a filesystem provider

The existing M5 ROMFS was intentionally created as a small recovery-store API before ZephyrOS had a VFS.

M9 preserves its proven low-level reader but exposes it through the filesystem-provider framework.

Conceptually:

```text
ROM page access
      |
      v
existing ROMFS format reader
      |
      v
fs/romfs
      |
      v
zfs_ops
      |
      v
VFS mount at /SYSTEM
```

ROMFS remains:

```text
read-only
resident in recovery ROM pages
available without SD
boot/recovery authoritative
```

Mutation operations return `EROFS`.

The current ROMFS format may remain flat. Mounted at `/SYSTEM`, its files simply appear directly in that directory.

There is no requirement in M9 to redesign the ROMFS format to add subdirectories.

VFS initializes before dynamic module loading. The ROMFS provider is statically available, registers with VFS during bootstrap, and is instantiated as the configured system filesystem at `/SYSTEM`.

Normal boot must therefore not require a separate `romfs_load_zmod()`-style path once VFS bootstrap is complete. After `/SYSTEM` is mounted, module and executable loading should consume files through generic VFS-backed source adapters.

A tiny direct ROM access primitive may still exist underneath the ROMFS provider, but it is not a parallel filesystem API.

---

## 14. `/SYSTEM` policy

`/SYSTEM` contains the minimum ROM-resident material required to:

```text
boot ZephyrOS
recover from external-storage failure
meaningfully diagnose the machine
```

The exact contents are build-time configuration, not VFS policy.

Typical contents may include:

```text
essential ZMODs
monitor.zex
shell.zex
diagnostic/recovery assets
fonts/resources required by the boot environment
```

Whether a particular utility belongs in `/SYSTEM` is a build/product decision.

VFS only knows that `/SYSTEM` is a configured mountpoint.

---

## 15. Generic block devices

Raw storage remains independently accessible from filesystem services.

The existing `ZDEV_STORAGE` class is renamed directly to:

```text
ZDEV_BLOCK
```

The numeric class value is preserved.

No compatibility alias is required as part of the architecture.

A block device exposes geometry and raw block I/O.

Conceptually:

```c
struct zblock_info {
    uint16_t block_size;
    uint32_t block_count;
    uint16_t max_transfer_blocks;
    uint16_t flags;
};

struct zblock_ops {
    int (*get_info)(void *state,
                    struct zblock_info *out);

    int (*read)(void *state,
                uint32_t lba,
                uint16_t count,
                void *buffer);

    int (*write)(void *state,
                 uint32_t lba,
                 uint16_t count,
                 const void *buffer);

    int (*flush)(void *state);

    int (*media_status)(void *state);
};
```

For the current hardware:

```text
sd-ioc.zmod
    |
    | consumes ZDEV_IOC
    v
ZDEV_BLOCK: sd.0
```

The SD block driver owns translation between generic block requests and the MCU raw-SD command/bulk protocol.

It does not implement FAT.

---

## 16. IOC remains transport only

This is an architectural invariant.

`drivers/ioc/` owns:

```text
SIO1 command lane
SIO1 bulk lane
READY/DONE lifecycle
transport sequencing
timeouts
recovery
semantic IOC status
```

It does not own:

```text
filesystem namespace
FAT semantics
file handles
directory traversal
block-cache policy
native path syntax
```

Both the raw SD block driver and the MCU-FAT filesystem may consume IOC.

Conceptually:

```text
                    ZDEV_IOC
                    /       \
                   /         \
                  v           v
          SD block driver   IOC-FAT filesystem
               |                  |
          ZDEV_BLOCK           zfs_ops
```

The current firmware feature set therefore does not define the long-term ZephyrOS storage architecture.

---

## 17. IOC-FAT filesystem provider

The current MCU already provides FatFs-backed file operations.

M9 exposes that facility as a resident ZMOD filesystem provider:

```text
iocfat.zmod
```

Its stack is:

```text
VFS
 |
 v
IOC-FAT filesystem provider
 |
 | FS2 filesystem protocol
 v
ZDEV_IOC
 |
 v
MCU FatFs
 |
 v
SD
```

`iocfat.zmod`:

```text
discovers ZDEV_IOC through the device API
initializes private provider state
registers itself with VFS
```

IOC-FAT owns:

```text
FS2 command vocabulary
FS2 directory/file tokens
generation/cookie handling
translation of MCU/FatFs errors
filesystem-specific command/bulk sequencing
```

It must not expose FS2 tokens to applications.

The current backend may retain FAT 8.3 component limits if that is what the deployed MCU firmware supports. Native VFS path semantics must not be designed around that limitation.

---

## 18. Future native filesystems

The block-device layer deliberately makes other implementations possible:

```text
VFS
 |
 +--> IOC-FAT
 |
 +--> native VFAT
 |       |
 |       v
 |    ZDEV_BLOCK
 |
 +--> Fuzix filesystem
         |
         v
      ZDEV_BLOCK
```

A future native VFAT implementation may parse FAT structures on the Z80 and consume `ZDEV_BLOCK`.

A future Fuzix filesystem may do the same.

Neither requires changes to:

```text
shell
native application ABI
VFS namespace
CWD semantics
IOC transport
```

Only the filesystem provider changes.

---

## 19. Z80-side file-data cache

M9 requires a local Z80-side cache.

The MCU's FatFs/internal cache is too small to substitute for a host-side cache because it does not avoid repeated Z80-to-MCU transport transactions for recently accessed file data.

The M9 cache is therefore a **file-data cache**, not a raw block cache.

Conceptually:

```text
                         VFS
                          |
                    file-data cache
                    512-byte pages
                          |
                 filesystem provider
                    /           \
               IOC-FAT       native VFAT
                  |               |
                 IOC          block cache
                                  |
                              ZDEV_BLOCK
```

The file-data cache is keyed conceptually by:

```text
mount/filesystem identity
file/object identity
aligned file offset
```

For example:

```text
(file X, offset 0000h-01FFh)
(file X, offset 0200h-03FFh)
(file Y, offset 0000h-01FFh)
```

Each cache slot contains at least:

```text
512-byte data block
valid-byte count
filesystem/object identity
aligned file offset or file-block number
valid flag
replacement state
generation/version marker
```

### 19.1 Cache slot geometry

The slot size is fixed at:

```text
512 bytes
```

The number of slots is build-time configurable and must be reported by memory-layout tooling.

Examples:

```text
4 slots = 2 KiB
8 slots = 4 KiB
```

The architecture does not pin the slot count before the real M9 bank-7 memory budget is measured.

### 19.2 Initial write policy

M9 should start with **write-through**, not write-back.

For reads:

```text
cache hit
    -> return locally

cache miss
    -> provider read
    -> fill slot
    -> return data
```

For writes:

```text
provider write
    -> confirmed successful completion
    -> update or invalidate overlapping cached data
```

No write is considered committed merely because bytes reached the IOC bulk lane.

Write-back caching may be added later if measurement justifies the extra recovery complexity.

---

## 20. Future raw block cache

A future native VFAT or Fuzix filesystem may benefit from caching raw filesystem sectors such as:

```text
FAT sectors
directory sectors
allocation metadata
data sectors
```

That is a separate layer:

```text
native filesystem
    |
    v
generic block cache
    |
    v
ZDEV_BLOCK
```

This block cache is distinct from the M9 VFS file-data cache.

Block caching must not be buried inside IOC.

---

## 21. Native process filesystem state

Filesystem state belongs to the process model.

Each process has at least:

```text
current working directory
descriptor table
```

The initial native environment starts at:

```text
/
```

A child inherits its parent's CWD.

A child does not initially inherit open file or directory descriptors.

When a child exits:

```text
close all descriptors owned by child
discard child FS context
restore parent unchanged
```

This keeps M9/M10 simple and postpones descriptor inheritance until redirection or pipes create a real need.

---

## 22. Descriptor table

Files and directory iterators share one per-process descriptor table.

M9 guarantees:

```text
8 descriptors per process
```

Each slot is tagged:

```text
FREE
FILE
DIRECTORY
```

These are native VFS descriptors only.

They are not Unix `stdin/stdout/stderr` descriptors; console I/O remains its own native syscall family in M9/M10.

A file-manager-style program can therefore hold several files and directory iterators open concurrently without requiring separate arbitrary limits.

The table size may grow later without changing filesystem semantics.

---

## 23. Current working directory

CWD is a VFS concept, not a filesystem-specific token exposed to applications.

Applications see canonical namespace paths such as:

```text
/
/SYSTEM
/GAMES
/CF/WORK
```

Internally, VFS may cache filesystem-private node handles for efficiency, but the semantic identity remains in the global namespace.

This lets `cd` cross filesystem boundaries without special shell behavior.

Example:

```text
cd /SYSTEM
cd /
cd /CF
```

The shell uses the same `chdir()` operation for all of them.

---

## 24. Native file handles

Applications receive small ZephyrOS descriptors.

Conceptually:

```text
application fd
      |
      v
VFS descriptor
      |-- owner PID
      |-- type: file or directory
      |-- mount
      |-- filesystem-private handle
      |-- access flags
      `-- uint32 current offset
```

File size and file position are 32-bit.

Individual read/write syscall lengths may remain 16-bit.

Files larger than 64 KiB are therefore handled naturally through repeated operations.

There is no native 128-byte-record abstraction.

---

## 25. Native metadata ABI v1

The first native metadata ABI should expose only broadly useful filesystem-neutral information.

### 25.1 Types

```c
#define ZFS_TYPE_FILE 1u
#define ZFS_TYPE_DIR  2u
```

### 25.2 Attributes

```c
#define ZFS_ATTR_READONLY 0x01u
#define ZFS_ATTR_HIDDEN   0x02u
#define ZFS_ATTR_SYSTEM   0x04u
```

There is no native `ARCHIVE` attribute.

The FAT archive bit is a legacy backup-software concept and is not useful enough to include in the ZephyrOS VFS contract.

### 25.3 Timestamps

Zephyr-80 has no RTC.

M9 therefore defines:

```c
#define ZFS_TIME_UNKNOWN 0u
```

A timestamp value of zero means:

```text
unknown / unavailable
```

The ABI still uses 32-bit timestamp fields so adding a real time source later does not require changing the structure.

M9 exposes:

```text
ctime
mtime
```

and does not expose `atime`.

ROMFS will normally report:

```text
ctime = 0
mtime = 0
```

unless the build system later chooses to stamp ROMFS entries.

IOC-FAT may translate FAT timestamps when available.

### 25.4 `stat()`

Conceptually:

```c
struct zfs_stat {
    uint8_t  type;
    uint8_t  attributes;
    uint16_t reserved;

    uint32_t size;
    uint32_t ctime;
    uint32_t mtime;
};
```

### 25.5 `readdir()`

Conceptually:

```c
struct zfs_dirent {
    uint8_t  type;
    uint8_t  attributes;
    uint16_t name_length;

    uint32_t size;
    uint32_t ctime;
    uint32_t mtime;

    char name[ZFS_NAME_MAX];
};
```

The ABI does not expose:

```text
inode numbers
FAT clusters
sector numbers
FS2 tokens
ROMFS offsets
uid/gid
Unix permission modes
access time
```

---

## 26. Native filesystem syscall family

M9 should expose a coherent native syscall family semantically equivalent to:

```text
FS_OPEN
FS_CLOSE
FS_READ
FS_WRITE
FS_SEEK
FS_STAT

FS_OPENDIR
FS_READDIR
FS_CLOSEDIR

FS_MKDIR
FS_RMDIR
FS_UNLINK
FS_RENAME

FS_CHDIR
FS_GETCWD
FS_STATFS
FS_SYNC
```

Paths are native hierarchical paths.

Syscall argument blocks should use bounded pointer/length fields rather than require the kernel to scan arbitrary user memory for an unbounded NUL terminator.

User-runtime wrappers may expose normal C strings.

---

## 27. `CLOSE` and `SYNC`

M9 requires both.

`close(fd)` means:

```text
flush pending state for that file
release provider handle
release VFS descriptor
```

`sync(fd)` means:

```text
flush dirty cached/provider state associated with that file
propagate required writes to the underlying provider/storage
keep the descriptor open
```

A filesystem-wide internal `syncfs()` operation may also exist for orderly kernel/provider management.

M9 does not require a Unix-style global user-facing `sync` command.

---

## 28. Byte-oriented I/O

Native ZephyrOS I/O is byte-oriented.

A typical read path is:

```text
application
    |
    v
SYS_FS_READ
    |
    v
VFS descriptor + offset
    |
    v
file-data cache
    |
    +--> hit -> return locally
    |
    `--> miss -> filesystem.read()
                   |
                   +--> IOC-FAT -> IOC bulk
                   |
                   `--> future FS -> block layer
```

EOF is a successful zero-byte read.

Short reads are valid.

A typical write advances the file offset only after the filesystem reports the operation committed successfully.

For IOC-FAT, the existing IOC write-confirmation semantics remain authoritative.

---

## 29. Transfer staging

The IOC bulk lane supports transfers up to 512 bytes.

M9 may therefore reserve one bounded bank-7 filesystem transfer buffer, normally 512 bytes, in runtime/system workspace.

Conceptually:

```text
application bank
      ^
      | checked bank-copy gate
      v
512-byte staging buffer
      |
      v
VFS/provider
      |
      v
IOC bulk or block layer
```

This is persistent kernel workspace, not a large automatic stack object.

The staging buffer is an implementation detail and is not part of the public filesystem ABI.

---

## 30. Filesystem errors

The native error namespace should distinguish common filesystem conditions.

M9 should add equivalents of:

```text
EEXIST
ENOTDIR
EISDIR
ENOTEMPTY
EROFS
ESTALE
EXDEV
```

`ESTALE` is useful for generation-aware FS2 handles.

`EXDEV` is required for rename across different mounted filesystems.

Exact numeric values are pinned when the syscall ABI is implemented.

---

## 31. Cross-filesystem operations

VFS defines cross-filesystem behavior explicitly.

A rename within one filesystem may call that filesystem provider's native `rename()`.

A rename across mounts, for example:

```text
/FOO.DAT -> /CF/FOO.DAT
```

returns:

```text
EXDEV
```

The M10 shell may then implement `mv` as:

```text
copy destination
close/sync destination successfully
unlink source
```

when appropriate.

VFS itself must not silently convert `rename()` into a copy.

This keeps policy in userland.

---

## 32. Boot sequence

VFS is a foundational kernel service and initializes immediately after the minimal kernel environment is established.

The boot sequence is intentionally split into phases.

### 32.1 Minimal kernel establishment

Before VFS, the kernel establishes only what is required to run kernel code safely:

```text
memory mode / bank state
kernel/common stacks
early direct SIO diagnostics
core interrupt state
```

No loadable module is required at this point.

### 32.2 VFS bootstrap phase

```text
kernel start
    |
    v
vfs_init()
    |
    v
register built-in filesystem providers
    |
    `--> ROMFS
    |
    v
read generated mount configuration
    |
    v
find the unique system=true entry
    |
    v
instantiate system filesystem
    |
    v
publish /SYSTEM
```

The system filesystem is the filesystem that makes the remainder of module-based boot possible.

For the initial Zephyr-80 system:

```text
provider   = romfs
source     = recovery
mountpoint = /SYSTEM
system     = true
```

The system filesystem provider must:

```text
be statically available in the kernel image
have no dependency on ZMODs
have no dependency on dynamically registered devices
have no dependency on a non-system filesystem
use only primitive facilities already available at bootstrap
```

Failure to instantiate the system filesystem is fatal and is reported through the early direct console.

### 32.3 Module-loading phase

Once `/SYSTEM` exists, boot assets are loaded through VFS:

```text
/SYSTEM/sio.zmod
/SYSTEM/ioc.zmod
/SYSTEM/hid.zmod
/SYSTEM/v9958.zmod
/SYSTEM/iocfat.zmod
...
```

Conceptually:

```text
VFS file
   |
   v
generic zmod_source.read()
   |
   v
module loader
```

The same source-neutral approach should later be used for ZEX loading.

Loadable modules may register:

```text
devices
filesystem providers
other resident services
```

For example:

```text
iocfat.zmod
    |
    +--> discovers ZDEV_IOC
    |
    +--> initializes private provider state
    |
    `--> vfs_register_filesystem("iocfat", ...)
```

### 32.4 Normal mount phase

After resident modules have loaded and their providers have registered, VFS instantiates the remaining configured mounts:

```text
/       -> IOC-FAT, if available
/CF     -> future provider
/SD2    -> future provider
...
```

For non-system mounts:

```text
required = true
    mount failure is a boot/configuration failure

required = false
    mount failure is tolerated
```

An optional root filesystem may therefore fail while `/SYSTEM` remains available for recovery and diagnosis.

### 32.5 VFS ready phase

Normal user filesystem syscalls become available only after the configured normal mount pass completes.

This prevents applications from observing a partially constructed namespace during boot.

The conceptual lifecycle is therefore:

```text
VFS_PHASE_BOOTSTRAP
    vfs_init()
    register built-in providers
    instantiate system filesystem
    publish /SYSTEM

VFS_PHASE_MODULES
    load resident modules through VFS
    modules register devices/providers

VFS_PHASE_MOUNT
    instantiate remaining configured mounts

VFS_PHASE_READY
    expose normal process filesystem services
```

An implementation does not need a literal phase enum, but the lifecycle and dependency ordering are architectural.

### 32.6 Failure behavior

System filesystem failure:

```text
/SYSTEM unavailable
    |
    v
cannot load required modules
    |
    v
early-console fatal error
```

Normal root filesystem failure:

```text
/ fails to mount
    |
    v
/SYSTEM remains available
    |
    v
recovery monitor / diagnostics remain usable
```

---

## 33. Build-time mount table generation

The build consumes:

```text
config/mounts.json
```

and generates a compact mount table suitable for the target.

Conceptually:

```c
static const struct vfs_mount_config system_mounts[] = {
    { "/SYSTEM", "romfs",  "recovery", VFS_MOUNT_SYSTEM   },
    { "/",       "iocfat", "ioc.0",    VFS_MOUNT_OPTIONAL }
};
```

This representation is illustrative, not mandatory.

The important properties are:

- configuration is centralized;
- mountpoints are reviewable;
- host tooling validates duplicates and malformed paths;
- VFS implementation contains no Zephyr-80-specific mount policy;
- adding `/CF` or `/SD2` later does not require editing VFS source.

---

## 34. M9 acceptance program

M9 should use an ordinary native test program such as:

```text
fstest.zex
```

loaded from `/SYSTEM`.

It exercises the public native filesystem ABI, not IOC or FS2 directly.

Acceptance covers at least:

```text
namespace:
    /
    /SYSTEM
    root mountpoint visibility
    mountpoint shadowing
    case-insensitive lookup
    absolute paths
    relative paths
    .
    ..
    CWD crossing between / and /SYSTEM

ROMFS:
    open/read/stat
    directory enumeration
    mutation returns EROFS

IOC-FAT:
    open/create/close
    read/write
    sync
    seek
    truncate
    stat
    mkdir/rmdir
    rename
    unlink
    statfs

sizes:
    1
    127
    128
    129
    511
    512
    513
    >64 KiB

cache:
    repeated-read hit
    miss/fill
    cross-slot reads
    write-through update/invalidate
    no stale data after write
    generation invalidation

error paths:
    missing file
    wrong object type
    descriptor exhaustion
    stale backend handle
    unavailable SD
    failed confirmed write
    cross-filesystem rename -> EXDEV

process lifecycle:
    child inherits CWD
    child CWD changes do not alter parent
    child descriptors close on exit
    repeated exec/exit leaks no handles
```

Physical hardware remains authoritative for:

```text
SD media behavior
IOC bulk transfers
media/reset handling
confirmed writes
cache behavior under real transport timing
```

---

## 35. M9 non-goals

M9 does not implement:

- runtime `mount` / `umount`;
- drive letters;
- CP/M USER areas;
- FCBs;
- CP/M extents;
- 128-byte record I/O;
- symbolic links;
- permissions/users/groups;
- pipes;
- descriptor inheritance;
- shell redirection;
- automatic filesystem probing;
- arbitrary nested mountpoints;
- hot-plug policy beyond clean failure detection;
- write-back file caching;
- generic raw block caching unless a native filesystem actually requires it.

These may be added later without changing the basic VFS model.

---

## 36. Personality boundary

Later personalities consume the native VFS rather than redefine native storage.

For example, a CP/M personality may synthesize CP/M filesystem semantics over whatever native directory it maps to:

```text
CP/M program
    |
    v
CP/M personality
    |
    | FCB / drive / USER / record translation
    v
native VFS
```

A personality might map:

```text
A: -> /SYSTEM/CPM
B: -> current native directory
C: -> /CF
```

and synthesize CP/M directory entries from VFS `readdir()` results.

That compatibility behavior does not belong in M9 VFS itself.

The native namespace remains:

```text
/
```

with no drive letters.

---

## 37. Architectural invariants

M9 preserves these rules:

1. **VFS owns the global namespace.**
2. **VFS initializes before dynamic module loading.**
3. **Mount topology is static JSON build configuration in M9.**
4. **Exactly one configured mount is the system filesystem.**
5. **The system filesystem provider is statically available and has no dependency on loadable modules or dynamically registered devices.**
6. **The system filesystem is instantiated before resident modules are loaded.**
7. **After the system filesystem is mounted, subsequent boot assets are loaded through VFS.**
8. **Native ZephyrOS has no drive letters.**
9. **Native path comparison is ASCII case-insensitive.**
10. **`ZOS_PATH_MAX = 128`.**
11. **`ZFS_NAME_MAX = 64`.**
12. **ROMFS is a filesystem provider mounted at `/SYSTEM`.**
13. **The normal writable SD FAT filesystem is mounted at `/`.**
14. **Filesystem providers do not know their external mountpoint.**
15. **IOC is transport, not a filesystem or block-device API.**
16. **`ZDEV_STORAGE` is renamed directly to `ZDEV_BLOCK`.**
17. **Raw SD access remains available through `ZDEV_BLOCK`.**
18. **IOC-FAT is a resident ZMOD filesystem provider registered with VFS.**
19. **MCU FatFs support is one filesystem backend, not the VFS architecture.**
20. **Future VFAT/Fuzix-style filesystems may consume `ZDEV_BLOCK` directly.**
21. **Native I/O is path-, descriptor-, and byte-oriented.**
22. **Each process has 8 VFS descriptors shared by files and directories.**
23. **M9 provides both `CLOSE` and `SYNC`.**
24. **Timestamp value zero means unknown/unavailable.**
25. **The native metadata ABI exposes file/dir type, readonly/hidden/system attributes, size, ctime, and mtime.**
26. **The FAT archive bit is not exposed by native VFS.**
27. **M9 includes a local 512-byte-slot Z80 file-data cache.**
28. **The file-data cache is initially write-through.**
29. **The number of file-data cache slots is build-time configurable and reported by tooling.**
30. **CP/M semantics remain confined to a later personality layer.**
31. **Failure of external storage must not remove `/SYSTEM` recovery access.**
---

## 38. Suggested source ownership

A possible implementation layout is:

```text
kernel/
    vfs/
        namespace.c
        path.c
        mount.c
        file.c
        dir.c
        cache.c
        syscall.c

    block/
        block.c
        cache.c          # optional/future raw-block cache

fs/
    romfs/
        ...

    iocfat/
        ...

    fat/                 # future
        ...

    fuzix/               # future
        ...

drivers/
    ioc/
        ...

    sd/
        ...

include/zephyros/
    vfs.h
    fs.h
    block.h

config/
    mounts.json
```

This is architectural guidance, not a requirement to create every file immediately.

Executable behavior remains outside `platform/zephyr80/`; platform files contain hardware facts/configuration only.

---

## 39. Summary

The resulting architecture is:

```text
                         kernel bootstrap
                                |
                                v
                              VFS
                    owns one namespace: /
                    paths / CWD / fd / cache
                                |
                     system=true mount
                                |
                                v
                            /SYSTEM
                              ROMFS
                                |
                      load resident ZMODs
                                |
                                v
                    register normal providers
                                |
                                v
          +---------------------+---------------------+
          |                                           |
          v                                           v
      /SYSTEM                                      /
       ROMFS                                    IOC-FAT
          |                                           |
          |                                           v
          |                                        ZDEV_IOC
          |                                           |
          |                                           v
          |                                          MCU
          |                                           |
          |                                           v
          |                                          SD
          |
          +---------------- future -------------------+
                                |
                         native VFAT / Fuzix
                                |
                                v
                           block cache
                                |
                                v
                           ZDEV_BLOCK
                                |
                                v
                           SD-over-IOC
                                |
                                v
                             ZDEV_IOC
```

This gives ZephyrOS a small but real filesystem framework without importing Unix complexity or CP/M drive semantics, while preserving room for new storage hardware, new filesystem implementations, future personalities, and more sophisticated caching later.

The system-filesystem rule also makes VFS part of the boot foundation rather than a service layered on top of module loading: the kernel mounts one dependency-free built-in filesystem first, then uses that filesystem to obtain the modules and providers required to construct the rest of the machine.
