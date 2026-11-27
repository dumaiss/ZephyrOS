/* KonaOS SYS_PERSONALITY (Zephyr-80 Mk2, abi/kos/xmem.h, docs/abi.md
 * section 8.4, architecture 2.7.6): the one-way handoff to another
 * machine whose 64 KiB space the program built in its object pages.
 * The body runs in cold set 1, beside the object table it resolves the
 * records through (kos_xmem_page); the switch that maps the pages and
 * jumps is fixed-region code (kernel/memory/mk2/dat_map.asm).
 */

#include "kos/syscall.h"
#include "kos/xmem.h"
#include "kos/errno.h"
#include "kos/ipc.h"
#include "kos/protocol.h"
#include "kos/ksound.h"
#include "process.h"
#include "kernel/memory/mk2/pages.h"
#include "kernel/ipc/ipc.h"
#include "kernel/irq/irq_private.h"
#include "kernel/syscall/rt.h"
#include "kernel/vfs/vfs.h"

#include "diag.h"
#include "xmem.h"

/* One driver call of the teardown, to the endpoint published at `name`
 * (none in this build: nothing to stop).  The lookup is another cold
 * set's, which cannot see this set's strings, so the name is copied to
 * the stack first. */
static void personality_call(const char *name, uint8_t op, uint8_t arg0,
			     uint16_t arg1)
{
	struct kos_ipc_frame frame;
	char rel[8];
	uint8_t endpoint;
	uint8_t i = 0u;

	do {
		rel[i] = name[i];
	} while (name[i++] != '\0');
	endpoint = vfs_ns_publication_endpoint(rel);
	if (endpoint == 0u) {
		return;
	}
	frame.op = op;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = arg0;
	frame.arg1 = arg1;
	(void)ipc_nucleus_call(endpoint, &frame);
}

/* Is `address` one of the switch routine's bytes? */
static uint8_t personality_in_switch(uint16_t address)
{
	return (uint8_t)(address >= (uint16_t)kos_personality_switch &&
			 address < (uint16_t)kos_personality_switch_end);
}

/* SYS_PERSONALITY: every record is checked and resolved to its page
 * before anything changes.  Then the teardown, masked from its first
 * step: the RT vector back to idle and the IO controller's RT clock
 * stopped (the release of `/dev/rtclock`'s exclusive hold), the sound
 * card silenced, every CTC channel stopped.  The serial lanes and the IO
 * controller's HID doorbell raise no Z80 interrupt (WR1 = 00h), and the
 * V9958 interrupts only while a program holds the display, so nothing
 * else is running.  The fixed-region switch maps the pages and jumps;
 * every other KonaOS state - processes, objects, drivers, the console -
 * is abandoned in place. */
void kos_personality_enter(uint16_t block)
{
	struct kos_personality_args *args =
		(struct kos_personality_args *)block;
	struct kos_personality_page list[KOS_PERSONALITY_PAGES];
	uint8_t pages[KOS_PERSONALITY_PAGES];
	uint8_t pid = process_current_pid();
	uint16_t entry;
	uint8_t i;

	if (!kos_syscall_block_ok(block, sizeof(*args)) ||
	    args->count != KOS_PERSONALITY_PAGES || args->reserved != 0u ||
	    !kos_syscall_buffer_ok(args->list, (uint16_t)sizeof(list))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}
	entry = args->entry;
	kos_pages_copy(kos_caller_map(), args->list, (const uint8_t *)0,
		       (uint16_t)list, (uint16_t)sizeof(list));
	for (i = 0u; i < KOS_PERSONALITY_PAGES; ++i) {
		pages[i] = kos_xmem_page(list[i].handle, list[i].page, pid);
		if (pages[i] == 0u || list[i].reserved != 0u) {
			kos_syscall_error(KOS_EINVAL);
			return;
		}
	}
	if (personality_in_switch((uint16_t)(entry - 2u)) ||
	    personality_in_switch((uint16_t)(entry - 1u))) {
		kos_syscall_error(KOS_EINVAL);
		return;
	}

	__asm__("di");
	kos_rt_quiesce();
	personality_call("/ioc", KOS_OP_RELEASE, KIOC_RT_OBJECT, 1u);
	personality_call("/sound", KSOUND_OP_RESET, 0u, 0u);
	platform_ctc_quiesce_all();
	kos_personality_switch(pages, entry);
}
