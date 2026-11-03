/* KonaOS serial-hold outcome program (card program, run from the
 * production shell by tests/test_serial.py, typed on the serial line
 * after the rescue gesture, so the console's tee is armed).
 *
 * One line per outcome on the console; the replay asserts the lines,
 * what reached the serial line and every SIO0/B port access:
 *
 *   - INFO on an ordinary handle: the flags, the ports and the baud rate;
 *   - serial_hold, and a second exclusive bind refused with -KOS_EBUSY;
 *   - direct I/O: a receive timeout, ENQ sent, the replay's answer
 *     received;
 *   - the driver's held answers on the ordinary handle: GETC -KOS_EAGAIN,
 *     POLL 0, READ 0, PUTC 0, WRITE its length - and the console's poll
 *     and read, which drain the serial line, and its PUTC, which the tee
 *     mirrors to it;
 *   - serial_write, then serial_release: the driver restores the channel;
 *   - a second hold the program's exit releases.
 *
 * Every line printed while the port is held is one console WRITE the tee
 * forwards to the driver, which drops and counts it.
 */

#include "kos.h"
#include "kos/serial.h"
#include "kos/protocol.h"

#define SERIAL_NAME "/dev/serial/0"
#define SERIAL_ENQ  0x05u

static unsigned char lane;
static char data[4];

static int lane_call(unsigned char handle, uint8_t op, uint8_t arg0,
		     uint16_t length, uint8_t direction,
		     struct kos_ipc_request *req)
{
	req->frame.op = op;
	req->frame.flags = 0u;
	req->frame.desc = KOS_IPC_DESC_NONE;
	req->frame.arg0 = arg0;
	req->frame.arg1 = length;
	req->buffer = length != 0u ? (void *)data : (void *)0;
	req->length = length;
	req->direction = direction;
	return call(handle, req);
}

int main(void)
{
	struct kos_ipc_request req;
	unsigned char type = 0u;
	unsigned int i;
	int handle;
	int result;
	int r_getc;
	int r_poll;
	int r_read;
	int r_putc;
	int r_write;

	handle = bind(SERIAL_NAME, 0u, &type);
	if (handle < 0 || type != KOS_TYPE_SERIAL) {
		printf("serial: FAIL bind %d\r\n", handle);
		return 1;
	}
	lane = (unsigned char)handle;
	result = lane_call(lane, KSIO_OP_INFO, 0u, 0u, KOS_IPC_DESC_IN, &req);
	printf("serial: info %d", result);
	for (i = 0u; i < KSIO_INFO_BAUD + 4u; ++i) {
		printf(" %02x", (unsigned int)req.frame.payload[i]);
	}
	printf("\r\n");

	result = serial_hold();
	printf("serial: hold %d\r\n", result);
	if (result < 0) {
		return 1;
	}
	handle = bind(SERIAL_NAME, KOS_BIND_EXCLUSIVE, &type);
	printf("serial: busy %d\r\n", handle);

	result = serial_getc_wait(1u);
	printf("serial: none %d\r\n", result);
	serial_rts(1u);
	(void)serial_putc(SERIAL_ENQ);
	result = serial_getc_wait(5u * SERIAL_TICKS_PER_SECOND);
	serial_rts(0u);
	printf("serial: got %02x\r\n", (unsigned int)result);

	r_getc = lane_call(lane, KSIO_OP_GETC, 0u, 0u, KOS_IPC_DESC_IN, &req);
	r_poll = lane_call(lane, KSIO_OP_POLL, 0u, 0u, KOS_IPC_DESC_IN, &req);
	r_read = lane_call(lane, KSIO_OP_READ, 0u, 4u, KOS_IPC_DESC_OUT, &req);
	r_putc = lane_call(lane, KSIO_OP_PUTC, 'p', 0u, KOS_IPC_DESC_IN, &req);
	data[0] = 'a';
	data[1] = 'b';
	data[2] = 'c';
	r_write = lane_call(lane, KSIO_OP_WRITE, 0u, 3u, KOS_IPC_DESC_IN, &req);
	printf("serial: ops %d %d %d %d %d\r\n", r_getc, r_poll, r_read, r_putc,
	       r_write);

	r_poll = console_poll();
	r_read = console_read(data, 4u);
	r_putc = lane_call(KOS_HANDLE_STDOUT, KCONSOLE_OP_PUTC, 'q', 0u,
			 KOS_IPC_DESC_IN, &req);
	printf("serial: console %d %d %d\r\n", r_poll, r_read, r_putc);

	result = serial_write("held", 4u);
	printf("serial: write %d\r\n", result);

	result = serial_release();
	printf("serial: release %d\r\n", result);

	result = serial_hold();
	printf("serial: hold2 %d\r\n", result);
	printf("serial: PASS\r\n");
	return 0;
}
