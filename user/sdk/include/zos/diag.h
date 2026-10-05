/* ZephyrOS SDK: recovery/diagnostic wrappers (SYS_DIAG_*).
 *
 * The M7 recovery facilities behind the native syscall gate.  The kernel
 * validates bank/range/port and rejects bank 7, addresses outside the
 * documented windows, the reserved field, and the latch-aliased ports
 * 00h-0Fh; rejected requests are side-effect free.  Results follow the
 * libzos convention: >= 0 on success (the read byte or a written
 * length), or -errno.
 */

#ifndef ZOS_SDK_DIAG_H
#define ZOS_SDK_DIAG_H

#include "zos/types.h"
#include "zos/diag.h"

/* {bank, mask} as returned by SYS_DIAG_BANK_INFO. */
struct zos_bank_info {
	unsigned char bank; /* current application bank 0-6       */
	unsigned char mask; /* bit N set = bank N is usable (7Fh) */
};

/* One byte in `address` of `bank` (read) / one byte written (write).
 * Read returns the byte; write returns 0. */
int zex_diag_mem_read(struct zos_diag_mem_read_args *req);
int zex_diag_mem_write(struct zos_diag_mem_write_args *req);

/* One byte from / to I/O `port`.  IN returns the byte; OUT returns 0. */
int zex_diag_io_in(struct zos_diag_io_in_args *req);
int zex_diag_io_out(struct zos_diag_io_out_args *req);

/* Write {bank, mask} to `out`; returns 2. */
int zex_diag_bank_info(struct zos_bank_info *out);

/* Write the eight-byte v1 record to `out`; returns 8. */
int zex_diag_system_info(struct zos_diag_system_info *out);

#endif /* ZOS_SDK_DIAG_H */
