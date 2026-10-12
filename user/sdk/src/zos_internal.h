/* ZephyrOS libzos internal: the shared argument-block overlay.
 *
 * The FS/exec wrappers and the extended-memory wrappers (packages 43,
 * 44a and 44b) build every packed block here and marshal it through the
 * shared low-window scratch defined in runtime.asm.  This header is
 * private to user/sdk/src and is never installed.
 */

#ifndef ZOS_SRC_INTERNAL_H
#define ZOS_SRC_INTERNAL_H

#include "zos/fs.h"
#include "zos/ipc.h"
#include "zos/syscall.h"
#include "zos/seg.h"
#include "zos/xmem.h"

union zos_syscall_block {
	struct zfs_path_args    path;
	struct zfs_open_args    open;
	struct zfs_io_args      io;
	struct zfs_seek_args    seek;
	struct zfs_stat_args    stat;
	struct zfs_readdir_args readdir;
	struct zfs_getcwd_args  getcwd;
	struct zfs_rename_args  rename;
	struct zfs_fd_args      fd;
	struct zos_exec_args    exec;
	struct zos_xmem_alloc_args xmem_alloc;
	struct zos_xmem_free_args  xmem_free;
	struct zos_xmem_io_args    xmem_io;
	struct zos_xmem_info_args  xmem_info;
	struct zos_xmem_map_args   xmem_map;
	struct zos_seg_info_args   seg_info;
	struct zos_seg_map_args    seg_map;
};

extern union zos_syscall_block zos_scratch;

/* Low-level gate stub (ipcgate.asm): one RST 30h with the request's
 * number and block.  Carry-set nucleus errors map to -errno. */
int zos_ipc_raw_call(struct zos_ipc_call_req *req);

/* One raw syscall through the shared gate stub: `number` with `block` as
 * the packed argument block.  Returns the syscall result or the negated
 * error code; the block is always this program's low-window scratch. */
int zos_ipc_raw(unsigned char number, void *block);

#endif /* ZOS_SRC_INTERNAL_H */
