/* Host shim of the KonaOS umbrella for the note-compiler host build
 * (tests/snd/host): snd_manual.c includes "kos.h" but uses only the
 * fixed-width types and the error codes.  Not the real SDK header. */

#ifndef SND_HOST_KOS_H
#define SND_HOST_KOS_H

#include "kos/types.h"

#define KOS_EINVAL  0x02
#define KOS_ENODEV  0x03
#define KOS_EBUSY   0x04
#define KOS_ENOSPC  0x06
#define KOS_ENOTSUP 0x07

#endif /* SND_HOST_KOS_H */
