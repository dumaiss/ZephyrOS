/* KonaOS target acceptance runner (test artifact).
 *
 * The single integration seam between the production kernel and the
 * acceptance tests.  kernel/main.c calls the hook below, after console
 * binding, only when KOS_TARGET_TEST_BUILD is defined; the concrete
 * acceptance-group selection lives here so production files never
 * contain self-test implementations (docs/architecture.md section 3).
 *
 * Each acceptance image selects exactly one group: the subsystem image
 * (the ROMFS reader), the process image (the process/EXE lifecycle), the
 * exe2 image (the EXE v2 loader outcomes: the same process selftest's
 * last group) or the IPC image (the resident-driver substrate).
 */

#include "kernel.h"

#if KOS_SUBSYS_SELFTEST
#include "romfs_selftest.h"
#endif
#if KOS_PROCESS_SELFTEST || KOS_EXE2_SELFTEST
#include "process_selftest.h"
#endif
#if KOS_IPC_SELFTEST
#include "mod.h"
#include "kos/ipc.h"
#endif

void kos_target_test_late(void)
{
#if KOS_SUBSYS_SELFTEST
	kos_run_romfs_selftest();
#endif
#if KOS_PROCESS_SELFTEST || KOS_EXE2_SELFTEST
	kos_run_process_selftest();
#endif
#if KOS_IPC_SELFTEST
	/* The six synthetic resident drivers are real driver images in
	 * the recovery store; the nucleus driver loader reads the boot
	 * manifest and brings them up in bank 6 before the boot launcher
	 * starts the client program (/system/ipccopy.exe).  A load failure
	 * is a boot error: the loader reported it on the early console. */
	if (driver_load_all(KOS_DRIVER_MANIFEST_NAME) != 0) {
		panic("driver loader failed");
	}
#endif
}

/* TEST-ONLY HOOK: routes acceptance-only syscall numbers to their bodies.
 * Production links neither these bodies nor this hook (the object
 * family is production code, so only an acceptance-only number reaches
 * this hook).  Returns non-zero when handled. */
uint8_t kos_target_test_syscall(uint8_t number, uint8_t bank, uint16_t block)
{
	(void)number;
	(void)bank;
	(void)block;
	return 0u;
}
