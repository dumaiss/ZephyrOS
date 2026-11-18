/* KonaOS libkos: the real-time interrupt (kos/rt.h).
 *
 * Two holds make the interrupt: the nucleus routes the NMI vector to the
 * handler (SYS_RT_REGISTER), and the IO controller driver runs the RT
 * clock for the exclusive holder of `/dev/rtclock` (KIOC_OP_RT_CLOCK).
 * Opening takes the route first so the first tick finds the handler;
 * closing stops the clock first.  A process exit releases both.
 */

#include "kos.h"
#include "kos/protocol.h"
#include "kos_internal.h"

/* The `/dev/rtclock` handle + 1; 0 while closed (BSS, no initializer). */
static unsigned char rt_clock_handle;
/* The lane gap rt_open asks for, microseconds; 0 = the controller's. */
static unsigned char rt_gap_us;

static int rt_clock_rate(unsigned char handle, unsigned int rate_hz)
{
	struct kos_ipc_request req;

	req.frame.op = KIOC_OP_RT_CLOCK;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = rt_gap_us;
	req.frame.arg1 = (uint16_t)rate_hz;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call(handle, &req);
}

static void rt_release(void)
{
	(void)kos_ipc_raw(KOS_SYSCALL_RT_RELEASE, &kos_scratch.rt_register);
}

int rt_open(const void *handler, unsigned int rate_hz)
{
	unsigned char type = 0u;
	int handle;
	int result;

	if (rt_clock_handle != 0u) {
		return -KOS_EBUSY;
	}
	if (rate_hz < KOS_RT_RATE_MIN || rate_hz > KOS_RT_RATE_MAX) {
		return -KOS_EINVAL;
	}
	kos_scratch.rt_register.handler = (uint16_t)handler;
	result = kos_ipc_raw(KOS_SYSCALL_RT_REGISTER,
			     &kos_scratch.rt_register);
	if (result < 0) {
		return result;
	}
	handle = bind("/dev/rtclock", KOS_BIND_EXCLUSIVE, &type);
	if (handle < 0) {
		rt_release();
		return handle;
	}
	if (type != KOS_TYPE_IOC) {
		(void)unbind((unsigned char)handle);
		rt_release();
		return -KOS_ENODEV;
	}
	result = rt_clock_rate((unsigned char)handle, rate_hz);
	if (result < 0) {
		(void)unbind((unsigned char)handle);
		rt_release();
		return result;
	}
	rt_clock_handle = (unsigned char)(handle + 1);
	return 0;
}

int rt_gap(unsigned int us)
{
	if (us > 255u) {
		return -KOS_EINVAL;
	}
	rt_gap_us = (unsigned char)us;
	return 0;
}

int rt_close(void)
{
	unsigned char handle;

	if (rt_clock_handle == 0u) {
		return 0;
	}
	handle = (unsigned char)(rt_clock_handle - 1u);
	rt_clock_handle = 0u;
	(void)rt_clock_rate(handle, 0u);
	(void)unbind(handle);
	rt_release();
	return 0;
}

int rt_info(struct kos_rt_info *out)
{
	if (out == (struct kos_rt_info *)0) {
		return -KOS_EINVAL;
	}
	kos_scratch.rt_info.buffer = (uint16_t)out;
	return kos_ipc_raw(KOS_SYSCALL_RT_INFO, &kos_scratch.rt_info);
}
