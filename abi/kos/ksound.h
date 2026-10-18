/* KonaOS SOUND driver protocol (public native ABI): `/dev/sound`, driver
 * class 9, protocol type SOUND (9).
 *
 * `/dev/sound`: the sound card's lifecycle, its description and a batched
 * write path.  The driver owns the hardware's reset and
 * keeps no copy of its registers, because a program holding the device
 * exclusively may write the chips directly; every reset recovers from any
 * state.
 *
 *   KSOUND_OP_INFO  (0)  no descriptor; the reply payload is a struct
 *                        ksound_info: the card's targets, their ports and
 *                        the PSG clock.
 *   KSOUND_OP_WRITE (1)  descriptor IN, arg0 = 0, arg1 = the byte count:
 *                        a sequence of struct ksound_write pairs
 *                        {target, value}, performed in order as fast as
 *                        the hardware allows.  The count must be even and
 *                        non-zero, and every target one the card has
 *                        (-KOS_EINVAL before any write otherwise).
 *                        Returns the bytes consumed.
 *   KSOUND_OP_RESET (2)  no descriptor: silence and reinitialize the card.
 *                        Returns 0.
 *
 * The endpoint serves KOS_OP_ACQUIRE (the exclusive hold: refused while
 * held; the card is reset) and KOS_OP_RELEASE (the hold's end resets the
 * card).  While a program holds it, the ports KSOUND_OP_INFO reports are
 * that program's to write directly; the release puts the card back to
 * silence whatever it left.  WRITE and RESET are served on any handle.
 */

#ifndef KOS_KSOUND_H
#define KOS_KSOUND_H

#include "kos/types.h"

#define KSOUND_OP_INFO  0u
#define KSOUND_OP_WRITE 1u
#define KSOUND_OP_RESET 2u

/* struct ksound_write targets: the PSGs by number, then the DAC. */
#define KSOUND_TARGET_PSG0 0u
#define KSOUND_TARGET_PSG1 1u
#define KSOUND_TARGET_PSG2 2u
#define KSOUND_TARGET_PSG3 3u
#define KSOUND_TARGET_DAC  4u

/* struct ksound_info flags. */
#define KSOUND_CAP_DIRECT_IO   0x01u /* the exclusive holder may OUT    */
#define KSOUND_CAP_BATCH_WRITE 0x02u /* KSOUND_OP_WRITE is served       */
#define KSOUND_CAP_DAC         0x04u /* the card has the DAC target     */

#define KSOUND_INFO_VERSION 1u
#define KSOUND_PSG_MAX      4u

/* KSOUND_OP_INFO's reply payload (24 bytes, the whole frame payload).
 * The PSG ports are listed one by one: they are contiguous on today's
 * card, and the ABI does not assume it.  dac_port is 0 without a DAC. */
struct ksound_info {
	uint8_t version;              /* KSOUND_INFO_VERSION              */
	uint8_t flags;                /* KSOUND_CAP_*                     */
	uint8_t psg_count;            /* 0..KSOUND_PSG_MAX                */
	uint8_t dac_port;             /* the DAC's port, or 0             */
	uint8_t psg_ports[KSOUND_PSG_MAX]; /* PSG n's port (n < count)   */
	uint32_t psg_clock_hz;        /* the PSGs' input clock            */
	uint8_t reserved[12];         /* 0                                */
};

/* One write of a KSOUND_OP_WRITE sequence. */
struct ksound_write {
	uint8_t target;               /* KSOUND_TARGET_*                  */
	uint8_t value;                /* the byte the target receives     */
};

#if defined(__SDCC)
_Static_assert(sizeof(struct ksound_info) == 24,
	       "ksound_info must fill the 24-byte frame payload");
_Static_assert(__builtin_offsetof(struct ksound_info, psg_ports) == 4 &&
	       __builtin_offsetof(struct ksound_info, psg_clock_hz) == 8,
	       "ksound_info layout");
_Static_assert(sizeof(struct ksound_write) == 2,
	       "ksound_write must be 2 packed bytes");
#endif /* __SDCC */

#endif /* KOS_KSOUND_H */
