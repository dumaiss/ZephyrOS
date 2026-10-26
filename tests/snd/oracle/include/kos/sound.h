/* Host shim of the SOUND driver surface for the oracles: the recorder
 * (ztr_trace.c / zvg_trace.c) provides the sound_* functions, so only
 * the records and prototypes the reference sources use are declared. */

#ifndef SND_ORACLE_KOS_SOUND_H
#define SND_ORACLE_KOS_SOUND_H

#include <stdint.h>

#define KSOUND_TARGET_PSG0 0u
#define KSOUND_TARGET_PSG1 1u
#define KSOUND_TARGET_PSG2 2u
#define KSOUND_TARGET_PSG3 3u
#define KSOUND_TARGET_DAC  4u

#define KSOUND_CAP_DIRECT_IO   0x01u
#define KSOUND_CAP_BATCH_WRITE 0x02u

#define KSOUND_INFO_VERSION 1u
#define KSOUND_PSG_MAX      4u

struct ksound_info {
	uint8_t version;
	uint8_t flags;
	uint8_t psg_count;
	uint8_t dac_port;
	uint8_t psg_ports[KSOUND_PSG_MAX];
	uint32_t psg_clock_hz;
	uint8_t reserved[12];
};

struct ksound_write {
	uint8_t target;
	uint8_t value;
};

int sound_open(unsigned char exclusive);
int sound_close(int handle);
int sound_info(int handle, struct ksound_info *info);
int sound_write(int handle, const struct ksound_write *writes,
		unsigned int count);
int sound_reset(int handle);
void sound_out(unsigned char port, unsigned char value);

#endif /* SND_ORACLE_KOS_SOUND_H */
