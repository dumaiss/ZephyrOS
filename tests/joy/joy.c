/* KonaOS game-controller outcome program: `/system/joy.exe`.
 *
 * Two uses:
 *
 *   `joy`        the owner's check with one or two pads: each pad's raw
 *                latch byte, its bits (D7..D0, 0 = pressed) and both
 *                readings, printed whenever the byte changes, until a key
 *                is pressed;
 *
 *   `joy stage`  the acceptance replay's run: the binds and their type,
 *                the INFO ports, the refusals (an exclusive bind, pad 2
 *                by name and by READ), then STAGE_READS steps.  At each
 *                step the replay sets both latches at stage_mark; the
 *                program then prints both pads through joy_read and
 *                joy_read_call and both readings of each byte.
 *
 * Every line is `joy: ...`; the exit status is 0 unless a pad could not
 * be opened or a read failed.
 */

#include "kos.h"
#include "kos/joy.h"
#include "kos/protocol.h"

#define STAGE_TAIL  "stage"
#define STAGE_READS 6u

static int pads[KJOY_PAD_MAX];

/* The replay's checkpoints: it stops at stage_mark's entry and reads
 * stage_step. */
volatile unsigned char stage_step;

void stage_mark(void)
{
}

static void mark(unsigned char step)
{
	(void)console_flush();
	stage_step = step;
	stage_mark();
}

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

static int raw_call(int handle, uint8_t op, uint8_t pad,
		    struct kos_ipc_request *req)
{
	req->frame.op = op;
	req->frame.flags = 0u;
	req->frame.desc = KOS_IPC_DESC_NONE;
	req->frame.arg0 = pad;
	req->frame.arg1 = 0u;
	req->buffer = (void *)0;
	req->length = 0u;
	req->direction = KOS_IPC_DESC_IN;
	return call((unsigned char)handle, req);
}

/* "URDLF" for the joystick reading, '-' where a bit is clear. */
static void dirs_text(uint8_t raw, char *out)
{
	static const char names[] = "URDLF";
	static const uint8_t masks[] = {
		JOY_UP, JOY_RIGHT, JOY_DOWN, JOY_LEFT, JOY_FIRE
	};
	uint8_t dirs = joy_dirs(raw);
	uint8_t i;

	for (i = 0u; i < 5u; ++i) {
		out[i] = (dirs & masks[i]) != 0u ? names[i] : '-';
	}
	out[5] = '\0';
}

static char key_char(uint8_t raw)
{
	int key = joy_key(raw);

	return key < 0 ? '-' : (char)key;
}

static int open_pads(void)
{
	uint8_t pad;

	for (pad = 0u; pad < KJOY_PAD_MAX; ++pad) {
		pads[pad] = joy_open(pad);
		if (pads[pad] < 0) {
			printf("joy: open %u failed %d\r\n", (unsigned int)pad,
			       pads[pad]);
			return 1;
		}
	}
	return 0;
}

/* `joy stage`. */
static int joy_stage(void)
{
	struct kos_ipc_request req;
	unsigned char type;
	char dirs[2][6];
	uint8_t direct[KJOY_PAD_MAX];
	uint8_t viacall[KJOY_PAD_MAX];
	unsigned int step;
	uint8_t pad;
	int handle;
	int result;

	for (pad = 0u; pad < KJOY_PAD_MAX; ++pad) {
		char path[sizeof("/dev/input/joy/0")];

		type = 0u;
		memcpy(path, "/dev/input/joy/0", sizeof(path));
		path[sizeof(path) - 2u] = (char)('0' + pad);
		handle = bind(path, 0u, &type);
		printf("joy: bind %u %d type %u\r\n", (unsigned int)pad,
		       handle >= 0 ? 0 : handle, (unsigned int)type);
		if (handle < 0) {
			return 1;
		}
		if (pad == 0u) {
			result = raw_call(handle, KJOY_OP_INFO, 0u, &req);
			printf("joy: info %d count %u ports %02x %02x\r\n",
			       result,
			       (unsigned int)req.frame.payload[KJOY_INFO_COUNT],
			       (unsigned int)req.frame.payload[KJOY_INFO_PORTS],
			       (unsigned int)
			       req.frame.payload[KJOY_INFO_PORTS + 1u]);
			result = raw_call(handle, KJOY_OP_READ, 2u, &req);
			printf("joy: read pad 2 %d\r\n", result);
		}
		(void)unbind((unsigned char)handle);
	}
	handle = bind("/dev/input/joy/0", KOS_BIND_EXCLUSIVE, &type);
	printf("joy: exclusive %d\r\n", handle >= 0 ? 0 : handle);
	if (handle >= 0) {
		(void)unbind((unsigned char)handle);
	}
	printf("joy: open 2 %d\r\n", joy_open(2u));
	if (open_pads() != 0) {
		return 1;
	}
	printf("joy: stray %02x\r\n", (unsigned int)joy_read(15));

	for (step = 1u; step <= STAGE_READS; ++step) {
		mark((unsigned char)step);
		for (pad = 0u; pad < KJOY_PAD_MAX; ++pad) {
			direct[pad] = joy_read(pads[pad]);
			result = joy_read_call(pads[pad], &viacall[pad]);
			if (result < 0) {
				printf("joy: read %u failed %d\r\n", step,
				       result);
				return 1;
			}
			dirs_text(direct[pad], dirs[pad]);
		}
		printf("joy: %u read %02x %02x call %02x %02x "
		       "dirs %s %s key %c %c\r\n", step,
		       (unsigned int)direct[0], (unsigned int)direct[1],
		       (unsigned int)viacall[0], (unsigned int)viacall[1],
		       dirs[0], dirs[1], key_char(direct[0]),
		       key_char(direct[1]));
	}
	for (pad = 0u; pad < KJOY_PAD_MAX; ++pad) {
		(void)joy_close(pads[pad]);
	}
	console_puts("joy: stage done\r\n");
	return 0;
}

/* `joy`: each pad's byte whenever it changes, until a key. */
static int joy_watch(void)
{
	uint8_t last[KJOY_PAD_MAX] = { 0u, 0u };
	char bits[9];
	char dirs[6];
	char key;
	uint8_t pad;
	uint8_t raw;
	uint8_t i;

	if (open_pads() != 0) {
		return 1;
	}
	console_puts("joy: press the pads; any key ends\r\n");
	for (;;) {
		for (pad = 0u; pad < KJOY_PAD_MAX; ++pad) {
			raw = joy_read(pads[pad]);
			if (raw == last[pad]) {
				continue;
			}
			last[pad] = raw;
			for (i = 0u; i < 8u; ++i) {
				bits[i] = (raw & (uint8_t)(0x80u >> i)) != 0u ?
					  '1' : '0';
			}
			bits[8] = '\0';
			dirs_text(raw, dirs);
			printf("joy: %u raw %02x bits %s dirs %s key %c\r\n",
			       (unsigned int)pad, (unsigned int)raw, bits,
			       dirs, key_char(raw));
		}
		if (console_read(&key, 1u) > 0) {
			return 0;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 1 && argv != (char **)0 && same(argv[0], STAGE_TAIL)) {
		return joy_stage();
	}
	return joy_watch();
}
