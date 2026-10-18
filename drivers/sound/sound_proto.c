/* KonaOS SOUND resident driver (drivers/sound/).
 *
 * The Afternoon Blend card behind the SOUND protocol of
 * abi/kos/ksound.h (class KOS_CLASS_SOUND, instance 0, version
 * KOS_PROTOCOL_VERSION), published as `/dev/sound`: four SN76489 PSGs at
 * E0h-E3h and a DAC at E4h.
 *
 * The driver is lifecycle plus a coarse path.  It owns the card's reset
 * and a batched write, and keeps no copy of the chips' registers: a
 * program holding the device exclusively may write them directly, so
 * every reset has to recover from any register state.
 *
 *   KSOUND_OP_INFO   the card description, descriptor-less;
 *   KSOUND_OP_WRITE  {target, value} pairs through a 64-byte DRIVER_IO
 *                    buffer, validated in full before the first OUT;
 *   KSOUND_OP_RESET  the reset routine.
 *
 * KOS_OP_ACQUIRE takes the exclusive hold (a second one is -KOS_EBUSY)
 * and resets the card; KOS_OP_RELEASE with arg1 != 0 ends a hold and
 * resets it.  WRITE and RESET are served on any handle.
 *
 * sound_io.asm owns every port access: the reset and the write loop.
 * The image links no multiply or divide runtime.
 */

#include "kos/errno.h"
#include "kos/ipc.h"
#include "kos/protocol.h"

#include "driver.h"
#include "hardware.h"

extern void sound_io_reset(void);
extern int16_t sound_io_write_sequence(uint8_t desc, uint16_t length);

const struct driver_api *sound_driver_api;
static uint8_t sound_exclusive;

/* KSOUND_OP_INFO's 24-byte reply payload (struct ksound_info): version,
 * the three capabilities, four PSGs at E0h-E3h, the DAC at E4h, the
 * 3,579,545 Hz PSG clock little-endian, and zero reserved bytes. */
static const uint8_t sound_info_record[KOS_IPC_PAYLOAD_SIZE] = {
	KSOUND_INFO_VERSION,
	(uint8_t)(KSOUND_CAP_DIRECT_IO | KSOUND_CAP_BATCH_WRITE |
		  KSOUND_CAP_DAC),
	KOS_SOUND_PSG_COUNT,
	KOS_SOUND_DAC_PORT,
	KOS_SOUND_PSG0_PORT,
	KOS_SOUND_PSG1_PORT,
	KOS_SOUND_PSG2_PORT,
	KOS_SOUND_PSG3_PORT,
	(uint8_t)(KOS_SOUND_PSG_CLOCK_HZ & 0xFFu),
	(uint8_t)((KOS_SOUND_PSG_CLOCK_HZ >> 8) & 0xFFu),
	(uint8_t)((KOS_SOUND_PSG_CLOCK_HZ >> 16) & 0xFFu),
	(uint8_t)((KOS_SOUND_PSG_CLOCK_HZ >> 24) & 0xFFu),
	0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};

static int16_t sound_info(struct kos_ipc_frame *frame)
{
	const uint8_t *src = sound_info_record;
	uint8_t *dst = frame->payload;
	uint8_t left = KOS_IPC_PAYLOAD_SIZE;

	do {
		*dst = *src;
		++dst;
		++src;
	} while (--left != 0u);
	return 0;
}

static int16_t sound_write(struct kos_ipc_frame *frame)
{
	uint16_t length = frame->arg1;

	if (frame->desc == KOS_IPC_DESC_NONE || length == 0u ||
	    (length & 1u) != 0u) {
		return (int16_t)-KOS_EINVAL;
	}
	return sound_io_write_sequence(frame->desc, length);
}

int16_t sound_driver_dispatch(struct kos_ipc_frame *frame)
{
	if (frame->flags != 0u) {
		return (int16_t)-KOS_EINVAL;
	}
	switch (frame->op) {
	case KSOUND_OP_INFO:
		return sound_info(frame);
	case KSOUND_OP_WRITE:
		return sound_write(frame);
	case KSOUND_OP_RESET:
		sound_io_reset();
		return 0;
	case KOS_OP_ACQUIRE:
		/* One object, one exclusive holder.  The new holder starts
		 * from silence: the program may write the ports directly. */
		if (sound_exclusive != 0u) {
			return (int16_t)-KOS_EBUSY;
		}
		sound_io_reset();
		sound_exclusive = 1u;
		return 0;
	case KOS_OP_RELEASE:
		/* Only the exclusive hold's end resets the card; an ordinary
		 * handle going away changes nothing the driver owns. */
		if (frame->arg1 != 0u) {
			sound_exclusive = 0u;
			sound_io_reset();
		}
		return 0;
	default:
		return (int16_t)-KOS_EINVAL;
	}
}

uint8_t sound_driver_init(const struct driver_boot *boot)
{
	sound_driver_api = boot->api;
	sound_io_reset();
	return 0u;
}
