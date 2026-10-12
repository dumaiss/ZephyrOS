/* KonaOS SDK: recovery/diagnostic wrappers (SYS_DIAG_*).
 *
 * The M7 recovery facilities behind the native syscall gate.  The kernel
 * validates bank/range/port and rejects bank 7, addresses outside the
 * documented windows, the reserved field, and the latch-aliased ports
 * 00h-0Fh; rejected requests are side-effect free.  Results follow the
 * libkos convention: >= 0 on success (the read byte or a written
 * length), or -errno.
 */

#ifndef KOS_SDK_DIAG_H
#define KOS_SDK_DIAG_H

#include "kos/types.h"
#include "kos/diag.h"

/* {bank, mask} as returned by SYS_DIAG_BANK_INFO. */
struct kos_bank_info {
	unsigned char bank; /* current application bank 0-6       */
	unsigned char mask; /* bit N set = bank N is usable (7Fh) */
};

/* One byte in `address` of `bank` (read) / one byte written (write).
 * Read returns the byte; write returns 0. */
int diag_mem_read(struct kos_diag_mem_read_args *req);
int diag_mem_write(struct kos_diag_mem_write_args *req);

/* One byte from / to I/O `port`.  IN returns the byte; OUT returns 0. */
int diag_io_in(struct kos_diag_io_in_args *req);
int diag_io_out(struct kos_diag_io_out_args *req);

/* Write {bank, mask} to `out`; returns 2. */
int diag_bank_info(struct kos_bank_info *out);

/* Write the eight-byte v1 record to `out`; returns 8. */
int diag_system_info(struct kos_diag_system_info *out);

#endif /* KOS_SDK_DIAG_H */
