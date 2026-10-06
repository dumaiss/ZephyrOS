/* ZephyrOS M10 design spike: auxiliary segment 2 (33c/33d).
 *
 * Constant data + BSS plus the far entries that prove syscalls work from
 * far code: an ordinary SYS_CONSOLE_WRITE, an FS_READ, and a recursive
 * SYS_FAR_CALL that must return ZOS_EBUSY.  All syscall argument blocks
 * are stack locals, i.e. in the auxiliary low window that a syscall from
 * far code can see; the console/read buffers are segment addresses in the
 * auxiliary bank, which the kernel must copy from the execution bank.
 */

#include "zos/types.h"
#include "zos/syscall.h"
#include "zos/fs.h"
#include "zos/xmem.h"

struct seg_req {
	unsigned char number;
	void         *block;
};

extern int seg_rst(struct seg_req *req);

static const uint16_t seg2_table[4] = { 0x1111u, 0x2222u, 0x3333u, 0x4444u };
static uint16_t seg2_acc;
static const char seg2_msg[] = "far code: console OK\r\n";

/* far entry 2: constant-data compute through the far-call frame. */
uint16_t seg2_get(uint8_t *frame)
{
	uint16_t r;

	seg2_acc = (uint16_t)(seg2_acc + seg2_table[frame[0] & 3u]);
	r = seg2_acc;
	frame[0] = (uint8_t)(r & 0xFFu);
	frame[1] = (uint8_t)(r >> 8);
	return r;
}

/* far entry 3: an ordinary syscall (console write) from far code.  The
 * message is copied to a stack buffer first: storing a const-symbol
 * address directly into a stack block makes SDCC emit a byte-fragmented
 * relocation, while a stack address needs none. */
uint16_t seg2_console(uint8_t *frame)
{
	struct seg_req req;
	struct zos_console_args args;
	char buf[sizeof(seg2_msg)];
	unsigned int i;
	int rc;

	for (i = 0; i < sizeof(seg2_msg); ++i) {
		buf[i] = seg2_msg[i];
	}
	args.buffer = (unsigned int)buf;
	args.length = (unsigned int)(sizeof(seg2_msg) - 1u);
	req.number = ZOS_SYSCALL_CONSOLE_WRITE;
	req.block = &args;
	rc = seg_rst(&req);
	frame[0] = (uint8_t)(rc & 0xFFu);
	frame[1] = (uint8_t)((rc >> 8) & 0xFFu);
	return (uint16_t)rc;
}

/* far entry 4: FS_READ from far code.  The fd arrives in the frame; the
 * read buffer is a stack local in the auxiliary low window. */
uint16_t seg2_fsread(uint8_t *frame)
{
	struct seg_req req;
	struct zfs_io_args args;
	uint8_t buf[8];
	unsigned int fd = (unsigned int)(frame[0] |
					 ((uint16_t)frame[1] << 8));
	int rc;

	args.fd = (uint8_t)fd;
	args.buffer = (unsigned int)buf;
	args.length = 8u;
	req.number = ZOS_SYSCALL_FS_READ;
	req.block = &args;
	rc = seg_rst(&req);
	if (rc < 0) {
		frame[0] = (uint8_t)(rc & 0xFFu);
		return (uint16_t)rc;
	}
	frame[0] = buf[0];
	frame[1] = buf[1];
	frame[2] = buf[2];
	frame[3] = buf[3];
	return (uint16_t)rc;
}

/* far entry 5: a recursive far call must be refused with ZOS_EBUSY. */
uint16_t seg2_recurse(uint8_t *frame)
{
	struct seg_req req;
	struct zos_far_call_args args;
	int rc;

	args.entry = 1;
	args.reserved = 0;
	args.in = 0;
	args.in_len = 0;
	args.out = 0;
	args.out_len = 0;
	req.number = ZOS_SYSCALL_FAR_CALL;
	req.block = &args;
	rc = seg_rst(&req);
	frame[0] = (uint8_t)(rc & 0xFFu);
	frame[1] = (uint8_t)((rc >> 8) & 0xFFu);
	return (uint16_t)rc;
}
