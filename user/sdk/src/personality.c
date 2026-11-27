/* KonaOS libkos: the one-way handoff to another machine personality
 * (SYS_PERSONALITY, Zephyr-80 Mk2).  Its own member, so only a program
 * that hands the machine over links it.  The block goes through the shared
 * low-window scratch (kos_internal.h); Mk1 answers -KOS_ENOSYS. */

#include "kos.h"
#include "kos_internal.h"

int personality(const struct kos_personality_page *pages, unsigned char count,
		unsigned int entry)
{
	kos_scratch.personality.list = (unsigned int)pages;
	kos_scratch.personality.count = count;
	kos_scratch.personality.reserved = 0u;
	kos_scratch.personality.entry = entry;
	return kos_ipc_raw(KOS_SYSCALL_PERSONALITY,
			   &kos_scratch.personality);
}
