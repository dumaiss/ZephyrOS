/* KonaOS SDK: the sound card (the SOUND driver protocol).
 *
 * `/dev/sound` is the resident sound driver (KOS_CLASS_SOUND, type
 * KOS_TYPE_SOUND; kos/ksound.h).  Two ways to make sound:
 *
 *   - batched writes: build a sequence of {target, value} pairs (a whole
 *     music tick, say) and send it with one sound_write - one call into
 *     the driver however many register writes it holds;
 *   - direct I/O: open the device exclusively, read the ports from
 *     sound_info and write them with sound_out.  The release (sound_close,
 *     or the program's exit) silences the card whatever it was left in.
 *
 * Timing is the program's: a write happens when it is sent, and the
 * timer's events (kos/event.h) pace it.
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_SOUND_H
#define KOS_SDK_SOUND_H

#include "kos/types.h"
#include "kos/errno.h"
#include "kos/ksound.h"

/* Bind `/dev/sound` and return the handle, or the negated error
 * (-KOS_ENOENT when the namespace has no such name, -KOS_ENODEV when the
 * build publishes no sound driver).  `exclusive` non-zero takes the card
 * over (KOS_BIND_EXCLUSIVE): the card is reset, and its ports are this
 * program's to write until the handle goes; a second exclusive open is
 * -KOS_EBUSY. */
int sound_open(unsigned char exclusive);

/* Give the handle back; an exclusive one silences the card. */
int sound_close(int handle);

/* Fill `*info` (struct ksound_info, kos/ksound.h): the targets, the
 * ports a direct writer uses and the PSG clock.  0 or a negative error. */
int sound_info(int handle, struct ksound_info *info);

/* Perform `count` writes in order.  Sequences longer than one descriptor
 * (KOS_IPC_DESC_MAX bytes) are sent in several calls.  Returns the writes
 * performed, or a negative error (-KOS_EINVAL for an unknown target, with
 * no write of that call performed). */
int sound_write(int handle, const struct ksound_write *writes,
		unsigned int count);

/* Silence and reinitialize the card.  0 or a negative error. */
int sound_reset(int handle);

/* One direct write: `value` to `port`, for an exclusive holder writing the
 * ports sound_info reported.  (C cannot name a port chosen at run time.) */
void sound_out(unsigned char port, unsigned char value);

#endif /* KOS_SDK_SOUND_H */
