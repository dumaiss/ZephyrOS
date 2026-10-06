/* ZephyrOS libzos internal: the shared argument-block overlay.
 *
 * The FS/exec wrappers build every packed block here and marshal it
 * through the shared low-window scratch defined in runtime.asm.  This
 * header is private to user/sdk/src and is never installed.
 */

#ifndef ZOS_SRC_INTERNAL_H
#define ZOS_SRC_INTERNAL_H

#include "zos/fs.h"
#include "zos/syscall.h"
#include "zos/xmem.h"

union zos_fs_block {
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
	struct zos_xmem_alloc_args   alloc;
	struct zos_xmem_free_args    free_args;
	struct zos_xmem_io_args      xmem_io;
	struct zos_xmem_info_args    info;
	struct zos_zex_seg_info_args seginfo;
	struct zos_far_call_args     far;
};

extern union zos_fs_block zos_scratch;

#endif /* ZOS_SRC_INTERNAL_H */
