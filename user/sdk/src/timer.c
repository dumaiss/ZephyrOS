/* KonaOS libkos: the monotonic clock client over the TIMER driver.
 *
 * The clock is the `/dev/timer` publication (abi/kos/protocol.h, class
 * KOS_CLASS_TIMER).  Both operations are descriptor-less: a reading is
 * four bytes of reply payload, so the request carries no buffer and the
 * syscall gate captures nothing.
 *
 * The count has 25.6 µs resolution and wraps after about 30.5 hours;
 * elapsed times are unsigned differences.
 *
 * Return convention matches kos/console.h: a non-negative result, or the
 * negated native error code.
 */

#include "kos.h"

#include "kos/protocol.h"

/* Consecutive readings without a full tick before a wait gives up on the
 * clock (see timer_wait_us). */
#define TIMER_STALL_READS 100000u

int timer_open(void)
{
	unsigned char type = 0u;
	int handle = bind("/dev/timer", 0u, &type);

	if (handle < 0) {
		return handle;
	}
	if (type != KOS_TYPE_TIMER) {
		(void)unbind((unsigned char)handle);
		return -KOS_EINVAL;
	}
	return handle;
}

int timer_now(int handle, uint32_t *now)
{
	struct kos_ipc_request req;
	int result;

	if (now == (uint32_t *)0) {
		return -KOS_EINVAL;
	}
	req.frame.op = KTIMER_OP_NOW;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	if (result < 0) {
		return result;
	}
	/* The count is little-endian, the Z80's own order: the four bytes
	 * are the value (shifting each into place costs SDCC far more). */
	{
		uint8_t *out = (uint8_t *)now;

		out[0] = req.frame.payload[0];
		out[1] = req.frame.payload[1];
		out[2] = req.frame.payload[2];
		out[3] = req.frame.payload[3];
	}
	return 0;
}

uint32_t timer_us(uint32_t counts)
{
	/* 25.6 µs = 25 + 3/5.  The multiplies are shifts and adds, cheaper
	 * than a 32-bit multiply, and the shared library provides the long
	 * division the last term needs. */
	return (counts << 4) + (counts << 3) + counts +
	       (((counts << 1) + counts) / 5u);
}

int timer_wait_us(int handle, uint32_t us)
{
	uint32_t start;
	uint32_t now;
	uint32_t counts;
	uint32_t last;
	uint32_t stalled = 0u;
	int result = timer_now(handle, &start);

	if (result < 0) {
		return result;
	}
	/* Microseconds to counts: us * 5 / 128, split so every partial
	 * product stays inside 32 bits. */
	counts = ((us >> 7) << 2) + (us >> 7) +
		 ((((us & 0x7Fu) << 2) + (us & 0x7Fu)) >> 7);
	last = start;
	for (;;) {
		result = timer_now(handle, &now);
		if (result < 0) {
			return result;
		}
		if ((now - start) >= counts) {
			return 0;
		}
		/* A clock that stops - a dead driver, a channel whose
		 * interrupt was lost - must not hang the caller: one tick
		 * has to arrive within this many readings. */
		if ((now - last) >= KOS_TIMER_COUNTS_PER_TICK) {
			last = now;
			stalled = 0u;
		} else if (++stalled >= TIMER_STALL_READS) {
			return -KOS_ETIMEDOUT;
		}
	}
}
