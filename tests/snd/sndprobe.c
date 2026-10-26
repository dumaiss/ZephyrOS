/* The sound-series harness self-test (tests/snd).
 *
 * A card program the harness replays (tests/test_snd_oracle.py): it
 * writes three marker-tick sequences through sound_out - the DAC marker
 * E4h/A5h, then three PSG0 bytes - with a calibrated interrupt-free
 * delay between them, so the harness's split and its T-state timing
 * both decode exactly.
 *
 * The delay (sndprobe_delay.asm) costs 30 + 13*n T-states, so the two
 * intervals differ by exactly 13 * (64 - 32) = 416 T-states whatever
 * the fixed write path around them costs.
 */

#include "kos.h"
#include "kos/sound.h"

extern void sndprobe_delay(uint8_t iterations);

static int tick_bytes(struct ksound_info *info, unsigned char b0,
		      unsigned char b1, unsigned char b2)
{
	sound_out(info->dac_port, 0xA5u);
	sound_out(info->psg_ports[0], b0);
	sound_out(info->psg_ports[0], b1);
	sound_out(info->psg_ports[0], b2);
	return 0;
}

int main(void)
{
	struct ksound_info info;
	int handle;

	handle = sound_open(1u);
	if (handle < 0) {
		printf("sndprobe: cannot acquire /dev/sound (%d)\r\n", handle);
		return 1;
	}
	if (sound_info(handle, &info) < 0) {
		printf("sndprobe: sound_info failed\r\n");
		(void)sound_close(handle);
		return 1;
	}
	(void)tick_bytes(&info, 0x81u, 0x1Du, 0x94u);
	(void)tick_bytes(&info, 0x82u, 0x2Du, 0x95u);
	(void)tick_bytes(&info, 0x83u, 0x3Du, 0x96u);
	/* The timing phase: marker, delay, marker, delay, marker.  The two
	 * delays run in identical C contexts, so the interval between the
	 * two marker pairs differs by exactly the loop delta
	 * (13 * (64 - 32) T-states).  The closing marker also frames the
	 * last tick: everything after it (the release's silence) is the
	 * trailing write batch. */
	sound_out(info.dac_port, 0xA5u);
	sndprobe_delay(32u);
	sound_out(info.dac_port, 0xA5u);
	sndprobe_delay(64u);
	sound_out(info.dac_port, 0xA5u);
	(void)sound_close(handle);
	printf("sndprobe: PASS\r\n");
	return 0;
}
