/* libsnd: the foreground service for every source (its own member, so a
 * ZTR-only program can call snd_ztr_service and link no ZVG code). */

#include "kos.h"
#include "kos/snd.h"
#include "snd_private.h"

int snd_service(void)
{
	/* The ZVG arm: one bounded chunk per call. */
	if (snd_zvg_state == SND_ZVG_PREPARED ||
	    snd_zvg_state == SND_ZVG_PLAYING ||
	    snd_zvg_state == SND_ZVG_UNDERRUN) {
		(void)snd_zvg_refill();
	}
	return snd_ztr_service();
}
