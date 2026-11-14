/* KonaOS SDK: key_get (kos/key.h), the waiting read.  Its own member, so
 * a program that only polls (libtui among them) links no input pacing. */

#include "kos.h"
#include "kos/key.h"

key_code_t key_get(void)
{
	key_code_t key;

	while ((key = key_poll()) == KEY_NONE) {
		idle_input();
	}
	idle_input_ready();
	return key;
}
