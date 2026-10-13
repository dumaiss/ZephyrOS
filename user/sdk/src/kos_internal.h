/* KonaOS libkos internal: the shared argument-block overlay.
 *
 * The FS/exec wrappers and the extended-memory wrappers build every
 * packed block here and marshal it through the
 * shared low-window scratch defined in runtime.asm.  This header is
 * private to user/sdk/src and is never installed.
 */

#ifndef KOS_SRC_INTERNAL_H
#define KOS_SRC_INTERNAL_H

#include "kos/fs.h"
#include "kos/ipc.h"
#include "kos/syscall.h"
#include "kos/seg.h"
#include "kos/xmem.h"

union kos_syscall_block {
	struct kfs_path_args    path;
	struct kfs_open_args    open;
	struct kfs_io_args      io;
	struct kfs_seek_args    seek;
	struct kfs_stat_args    stat;
	struct kfs_readdir_args readdir;
	struct kfs_getcwd_args  getcwd;
	struct kfs_rename_args  rename;
	struct kfs_fd_args      fd;
	struct kos_exec_args    exec;
	struct kos_xmem_alloc_args xmem_alloc;
	struct kos_xmem_free_args  xmem_free;
	struct kos_xmem_io_args    xmem_io;
	struct kos_xmem_info_args  xmem_info;
	struct kos_xmem_map_args   xmem_map;
	struct kos_seg_info_args   seg_info;
	struct kos_seg_map_args    seg_map;
};

extern union kos_syscall_block kos_scratch;

/* Low-level gate stub (ipcgate.asm): one RST 30h with the request's
 * number and block.  Carry-set nucleus errors map to -errno. */
int kos_ipc_raw_call(struct kos_ipc_call_req *req);

/* One raw syscall through the shared gate stub: `number` with `block` as
 * the packed argument block.  Returns the syscall result or the negated
 * error code; the block is always this program's low-window scratch. */
int kos_ipc_raw(unsigned char number, void *block);

#endif /* KOS_SRC_INTERNAL_H */
