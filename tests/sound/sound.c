/* KonaOS sound-driver outcome program (card program, run from the
 * production shell by tests/test_sound.py).
 *
 * The program exercises the SOUND protocol through the SDK and prints one
 * line per outcome; the replay beside this file asserts the lines and the
 * emulated card's exact write sequence:
 *
 *   - INFO's whole 24-byte record;
 *   - a batched write performing exactly its pairs, in order;
 *   - an unknown target and an odd byte count writing nothing;
 *   - a sequence longer than one descriptor (4,100 bytes) landing whole;
 *   - an exclusive open resetting the card, and a second one refused
 *     with -KOS_EBUSY;
 *   - sound_out reaching the ports under the exclusive hold;
 *   - a close and an exit without a close both resetting the card.
 *
 * The final tone is written on an exclusive handle that is still held
 * when main returns: the process exit's release is the last reset.
 */

#include "kos.h"
#include "kos/sound.h"

/* 2,050 pairs: 4,100 bytes, so the SDK sends one full descriptor and the
 * remaining two pairs. */
#define BIG_COUNT 2050u

static struct ksound_write big[BIG_COUNT];

static void print_info(const struct ksound_info *info)
{
	const unsigned char *bytes = (const unsigned char *)info;
	unsigned int i;

	printf("sound: info");
	for (i = 0u; i < sizeof(*info); ++i) {
		printf(" %02x", (unsigned int)bytes[i]);
	}
	printf("\r\n");
}

int main(void)
{
	struct ksound_info info;
	struct ksound_write batch[4];
	struct ksound_write bad;
	struct kos_ipc_request req;
	unsigned int i;
	int handle;
	int exclusive;
	int result;

	handle = sound_open(0u);
	if (handle < 0) {
		printf("sound: FAIL open %d\r\n", handle);
		return 1;
	}
	result = sound_info(handle, &info);
	if (result < 0) {
		printf("sound: FAIL info %d\r\n", result);
		return 1;
	}
	print_info(&info);

	batch[0].target = KSOUND_TARGET_PSG0;
	batch[0].value = 0x8Eu;
	batch[1].target = KSOUND_TARGET_PSG3;
	batch[1].value = 0xF1u;
	batch[2].target = KSOUND_TARGET_DAC;
	batch[2].value = 0x80u;
	batch[3].target = KSOUND_TARGET_PSG1;
	batch[3].value = 0x9Fu;
	result = sound_write(handle, batch, 4u);
	printf("sound: batch %d\r\n", result);

	bad.target = (unsigned char)(KSOUND_TARGET_DAC + 1u);
	bad.value = 0x55u;
	result = sound_write(handle, &bad, 1u);
	printf("sound: unknown %d\r\n", result);

	/* The SDK sends whole pairs, so the odd byte count goes through the
	 * raw call block: three bytes of a four-byte buffer. */
	req.frame.op = KSOUND_OP_WRITE;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 3u;
	req.buffer = batch;
	req.length = 4u;
	req.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)handle, &req);
	printf("sound: odd %d\r\n", result);

	for (i = 0u; i < BIG_COUNT; ++i) {
		big[i].target = (unsigned char)(i & 3u);
		big[i].value = (unsigned char)(i ^ 0x5Au);
	}
	result = sound_write(handle, big, BIG_COUNT);
	printf("sound: big %d\r\n", result);

	result = sound_close(handle);
	printf("sound: close %d\r\n", result);

	exclusive = sound_open(1u);
	if (exclusive < 0) {
		printf("sound: FAIL exclusive %d\r\n", exclusive);
		return 1;
	}
	printf("sound: exclusive ok\r\n");
	result = sound_open(1u);
	printf("sound: busy %d\r\n", result);

	sound_out(info.psg_ports[0], 0xABu);
	sound_out(info.dac_port, 0x55u);
	printf("sound: direct\r\n");

	result = sound_close(exclusive);
	printf("sound: release %d\r\n", result);

	exclusive = sound_open(1u);
	if (exclusive < 0) {
		printf("sound: FAIL exclusive2 %d\r\n", exclusive);
		return 1;
	}
	printf("sound: exclusive2 ok\r\n");
	batch[0].target = KSOUND_TARGET_PSG0;
	batch[0].value = 0x8Eu;
	batch[1].target = KSOUND_TARGET_PSG0;
	batch[1].value = 0x00u;
	result = sound_write(exclusive, batch, 2u);
	printf("sound: final %d\r\n", result);

	printf("sound: PASS\r\n");
	return 0;
}
