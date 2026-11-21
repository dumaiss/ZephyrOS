/* KonaOS libsnd: the card handle the members share (user/sdk/snd).
 *
 * Its own member, so the PCM player can use a session's card - or find
 * there is none and hold the card itself - without linking the session's
 * PSG engine (snd.c and what it calls).
 */
#include "kos.h"
#include "snd_private.h"

/* The session's exclusive /dev/sound (snd_open), or 0. */
int snd_handle;

/* snd_close's call into an open PCM stream (snd_pcm.c sets it at open). */
void (*snd_pcm_closer)(void);
