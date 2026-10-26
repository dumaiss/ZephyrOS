/* Host shim for the libsnd note compiler (tests/snd/host).
 *
 * Provides the globals and the assembly entry stubs snd_manual.c and
 * snd.c reach, so the C compiler half builds untouched with the host
 * compiler; main runs the compile scenarios tests/test_snd.py pins
 * against its own reference.  The engine state the stubs stand in for
 * is just memory here.
 */

#include <stdio.h>

#include "snd_private.h"

int snd_handle = 1;
uint8_t snd_output_mode;
uint16_t snd_manual_rate;
const uint16_t *snd_custom_table;
uint16_t snd_reserved_voices;
uint16_t snd_active_rate;
unsigned int snd_overruns;

uint16_t snd_desired_period[SND_CHIP_COUNT][3];
uint8_t snd_desired_atten[SND_CHIP_COUNT][4];
uint8_t snd_desired_noise[SND_CHIP_COUNT];

void snd_load_ports(const void *info)
{
	(void)info;
}

void snd_shadow_invalidate(void)
{
}

void snd_output_flush(void)
{
}

void snd_voice_install(uint8_t voice)
{
	snd_voice_active[voice] = 1u;
	snd_voice_level[voice] = 0u;
}

void snd_voice_clear(uint8_t voice)
{
	snd_voice_active[voice] = 0u;
	snd_voice_level[voice] = 0u;
	snd_desired_atten[voice >> 2][voice & 3u] = 15u;
}

void snd_noise_apply(uint8_t noise_voice, uint8_t control)
{
	snd_desired_noise[noise_voice >> 2] =
		(uint8_t)(0xE0u | control);
}

static void dump_program(uint8_t voice)
{
	unsigned int i;
	unsigned int end = SND_PROGRAM_BYTES;

	for (i = SND_PROGRAM_BYTES; i > 0u; --i) {
		if (snd_programs[voice][i - 1u] != 0u) {
			end = i;
			break;
		}
	}
	printf("program %u:", voice);
	for (i = 0u; i < end; ++i) {
		printf(" %02x", snd_programs[voice][i]);
	}
	printf("\n");
}

int main(void)
{
	snd_instrument_simple_t pluck = {
		.kind = SND_INSTR_TONE, .peak_level = 200, .gate_ticks = 200,
		.attack_ticks = 2, .decay_ticks = 2, .sustain_level = 120,
		.release_ticks = 2, .noise_mode = 0, .noise_rate = 0,
	};
	int result;

	snd_define_instrument(0, &pluck);
	result = snd_note_for(0u, SND_C4, 8u, 0u);
	printf("note %d\n", result);
	dump_program(0u);
	result = snd_note_on(1u, SND_A4, 0u);
	printf("note_on %d\n", result);
	snd_voice_level[1u] = 150u;
	result = snd_note_off(1u);
	printf("note_off %d\n", result);
	dump_program(1u);
	/* the refusals */
	printf("bad voice %d\n", snd_note_on(3u, SND_C4, 0u));
	printf("bad note %d\n", snd_note_on(0u, 96u, 0u));
	snd_reserved_voices = (uint16_t)(1u << 1u);
	printf("reserved %d\n", snd_note_on(1u, SND_C4, 0u));
	snd_reserved_voices = 0u;
	snd_desired_noise[0u] = 0xE7u;
	printf("rate3 tone2 %d\n", snd_note_on(2u, SND_C4, 0u));
	snd_voice_active[2u] = 1u;
	printf("rate3 apply %d\n",
	       snd_set_noise(3u, SND_NOISE_WHITE, 3u));
	snd_voice_active[2u] = 0u;
	printf("rate3 free %d\n", snd_set_noise(3u, SND_NOISE_WHITE, 3u));
	printf("noise byte %02x\n", snd_desired_noise[0u]);
	return 0;
}
