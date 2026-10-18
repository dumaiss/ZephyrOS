# KonaOS Namespace (VFS)

How the nucleus implements the namespace: the root router, the three
providers, the handle table, the CWD and the workspace. The rules are in
`docs/architecture.md` section 4.10; the syscalls are in `docs/abi.md`
section 6.

## 1. Source layout

All of it is nucleus code in the cold half, except the workspace:

| file | part |
|---|---|
| `kernel/vfs/router.c` | the root router: bind, unbind and call dispatch, namespace operations, enumeration of `/`, `chdir`, init |
| `kernel/vfs/path.c` | path normalization and the shared component compare |
| `kernel/vfs/handles.c` | the handle table, the standard handles, process start and exit, the CWD slots |
| `kernel/vfs/workspace.c` | the 512-byte workspace (fixed core) |
| `kernel/vfs/dev.c` | `/dev`: the DEVICE provider over the publication table |
| `kernel/vfs/fs.c` | `/fs`: the provider that forwards to the filesystem driver |
| `kernel/vfs/system.c` | `/system`: the ROMFS provider |
| `kernel/vfs/vfs.h` | the one private interface |
| `kernel/syscall/object.c` | the syscall bodies: bind, call, unbind, ns_enum, chdir, getcwd |

Name comparison is one shared helper (`kos_ascii_equal()`,
`kernel/lib/name.c`) used by the VFS and ROMFS: `A-Z == a-z`, every other
byte exact, no Unicode folding.

## 2. Paths

`vfs_path_normalize_at(base, path, out)` turns a path into an absolute
canonical one: relative paths join the base (the process's CWD), repeated
and trailing slashes collapse, `.` is dropped, `..` removes the last
component, and `..` above `/` stays at `/`. Case is preserved: the user's
spelling is kept, and comparisons fold. `KOS_PATH_MAX` (128) and
`KFS_NAME_MAX` (64) include the NUL, so the longest path is 127 bytes and
the longest component 63; an over-long path or a too-small buffer is
`KOS_EINVAL`. The function keeps no state, so it is safe on any stack.

## 3. The root router

`bind`, `ns_enum`, `chdir` and the namespace operations normalize the path
and route by the first component, case-insensitively:

| root | provider | owns |
|---|---|---|
| `/fs` | `fs.c`, forwarding over IPC to the filesystem driver the build selected | everything below `/fs` |
| `/system` | `system.c`, ROMFS, in-process, usable before any driver | everything below `/system` |
| `/dev` | `dev.c`, the publication table | everything below `/dev` |

The provider receives the rest of the path and the router never interprets
it. `/` lists the three roots and is not bindable; any other root-level
name has no owner and is `-KOS_ENOENT`. A name that only starts like a root
(`/systemx`) is not that root.

## 4. The providers

**`/fs`.** The provider forwards to the filesystem driver's endpoint, taken
from the host-built `/fs` publication. A `bind` below `/fs` becomes the
driver's open; `stat`, `mkdir`, `rmdir`, `unlink` and `rename` become `FS`
protocol calls; enumeration and `chdir` validation are forwarded too. The
provider has no filesystem knowledge of its own. Paths reach the driver
through the workspace, which is in the fixed core because the driver reads
it with its own window open. With no card, the driver answers
`-KOS_ENODEV` (`docs/devices.md` section 8).

**`/system`.** `bind("/system/name", flags)` finds the flat name with
`romfs_lookup_ci()` and keeps the file instance inline in the handle entry,
so no second table is needed. The FILE operations are served in-process by
a nucleus endpoint through the same call path as a bank-6 endpoint, reading
through the common copy gate in chunks: `KFILE_OP_READ`, `KFILE_OP_SEEK` and
`KFILE_OP_META` work, and `KFILE_OP_WRITE` is `-KOS_EROFS` (the bind's write
flags are refused first). `bind("/system", 0)` yields the namespace handle
the name operations use: `stat` works and every mutation is `-KOS_EROFS`.

**`/dev`.** Names come from the publication table the loader installs
(`docs/devices.md` section 11). The provider derives the grouping names
(`/dev/input`, `/dev/serial`) from the names it holds; they enumerate as
containers and refuse to bind with `-KOS_EISDIR`. Lookup is a table scan.

## 5. The handle table

One global table of `VFS_HANDLE_MAX` (12) entries in the cold half. Each
entry records:

```text
owner PID
8-bit protocol type
endpoint (a bank-6 driver endpoint or a nucleus one)
the provider's object identity (or, for /system, the file instance itself)
the bound mark and the bind flags
```

- `bind` returns a handle of `KOS_HANDLE_FIRST_BOUND` (3) or more. A full
  table is `KOS_EMFILE`. `KOS_BIND_EXCLUSIVE` asks a DEVICE for a takeover
  (`docs/devices.md` section 14): the flag is stored in the entry and the
  provider is asked first, so a refused acquire leaves the table unchanged.
- `call` and `unbind` accept only a bound handle the calling process owns;
  anything else is `KOS_EINVAL`.
- Releasing a DEVICE handle is a notification, not a close: the entry goes
  back to the pool and, when the publishing endpoint declared it, the
  driver is told the handle is gone (`vfs_ns_release_entry`,
  `vfs_device_notify_release`).
- Handles 1 and 2 are the standard handles, bound to `/dev/console` for
  each process at start (`vfs_ns_standard_handles()`), when the build
  publishes a console. `bind` never returns them, and `unbind` refuses them.
- The table is global because the machine is single-tasking: a suspended
  parent's handles stay allocated while its child runs.
- Process exit (`vfs_process_exit()`) unbinds every handle the process
  owns; a file's unbind lets the filesystem driver close and flush it, and
  a device's release notification reaches the driver on this path too,
  which is what restores a device a program took over before it exited.

## 6. The CWD

Each active process has one CWD slot (`KOS_EXEC_MAX_DEPTH` slots of one
owner byte and a 128-byte canonical path). `vfs_process_start()` gives a
new process a copy of its parent's CWD and its standard handles; it is
idempotent. `chdir` normalizes the path, asks the owning provider whether
it is a traversable location (`vfs_ns_dir_check()`), and stores the
canonical path; it binds nothing. A child's `chdir` never changes the
parent's slot.

## 7. The workspace

`vfs_workspace` is one 512-byte buffer in the fixed core with two views
that are never live at once:

```text
path view      four 128-byte slots: IN_A, NORM_A, IN_B, NORM_B
               (the caller's path and its normalized form; B for a second path)
staging view   all 512 bytes: result assembly for the gate
```

| operation | view |
|---|---|
| `BIND`, `NS_ENUM`, `CHDIR`, namespace `CALL`s, `EXEC`'s path | path view |
| `GETCWD` | path view (one slot), then staging |
| a `CALL` carrying file data | neither: the data moves by descriptor copies |

Acceptance builds (`KOS_VFS_WS_GUARD=1`) fault if the two views overlap in
use; production compiles the guard out.

## 8. Boot

`vfs_init()` runs before any driver is placed: it validates the store
through the ROMFS reader, clears the handle table and the CWD slots, and
sets up the in-process FILE endpoint. From then on `/system` works. `/dev`
and `/fs` answer once the driver loader has installed the publications.
`vfs_phase()` reports `READY` from the end of `vfs_init()`; `EXEC` refuses
with `KOS_EBUSY` before that.

## 9. Tests

- `tests/test_ns_selftest.py`, `tests/test_ns_selftest_sim.py`: the
  `ns-selftest` image boots SIO, the console and a synthetic driver that
  publishes `/dev/input/keyboard` and `/dev/video`, then a client
  (`/system/ns.exe`) that checks the standard handles, the publication
  types, grouping-name refusals, a call through a handle, a `/system` read,
  seek and metadata, `KOS_EROFS`, table exhaustion and release, paged
  enumeration of `/`, `/dev` and `/system`, and a child's standard handles.
- `tests/test_fs_selftest_sim.py`: files on `/fs` and nested exec, with and
  without a card.
- `tests/test_hwtest_sim.py`: the namespace stage of the production
  exerciser against the real driver set.
