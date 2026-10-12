/* KonaOS target acceptance runner (test artifact).
 *
 * The single integration seam between the production kernel and the
 * acceptance tests.  kernel/main.c calls the two hooks below only when
 * KOS_TARGET_TEST_BUILD is defined; the concrete acceptance-group
 * selection lives here so production files never contain self-test
 * implementations (docs/architecture.md section 3).
 *
 * Boot phases:
 *   early  before the boot program load (syscall gate, IRQ executive)
 *   late   after console binding (ROMFS reader, process/EXE, loader)
 *
 * Each acceptance image selects exactly one group: the subsystem image
 * (relocated subsystem mechanism selftests), the device/console subsystem images,
 * or the M9 VFS images.
 */

#include "kernel.h"

#if KOS_SUBSYS_SELFTEST
#include "syscall_selftest.h"
#include "irq_selftest.h"
#include "romfs_selftest.h"
#include "process_selftest.h"
#endif
#if KOS_IOC_SELFTEST
#include "ioc_selftest.h"
#endif
#if KOS_HID_SELFTEST
#include "hid_selftest.h"
#endif
#if KOS_VIDEO_SELFTEST
#include "video_selftest.h"
#endif
#if KOS_CONSOLE_INTEGRATION_SELFTEST
#include "console_integration_selftest.h"
#endif
#if KOS_NUCLEUS_SELFTEST
#include "nucleus_selftest.h"
#endif
#if KOS_IPC_SELFTEST
#include "mod.h"
#include "kos/ipc.h"
#endif

void kos_target_test_early(void)
{
#if KOS_SUBSYS_SELFTEST
	kos_run_syscall_selftest();
	kos_run_irq_selftest();
#endif
#if KOS_NUCLEUS_SELFTEST
	kos_run_nucleus_selftest();
#endif
}

void kos_target_test_late(void)
{
#if KOS_SUBSYS_SELFTEST
	/* ROMFS reader, then the process/EXE lifecycle. */
	kos_run_romfs_selftest();
	kos_run_process_selftest();
#endif
#if KOS_IOC_SELFTEST
	/* The console-integration image carries the device diagnostics
	 * (work package 42d): IOC transport, HID input, V9958 video, then
	 * the integration pass itself.  Each is a bank-7 client of the
	 * resident drivers; the transcript runs on the early SIO. */
	kos_run_ioc_selftest();
#endif
#if KOS_HID_SELFTEST
	kos_run_hid_selftest();
#endif
#if KOS_VIDEO_SELFTEST
	kos_run_video_selftest();
#endif
#if KOS_CONSOLE_INTEGRATION_SELFTEST
	kos_run_console_integration_selftest();
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
 * Production links neither these bodies nor this hook (package 42b: the
 * object family is production code, so only an acceptance-only number
 * reaches this hook).  Returns non-zero when handled. */
uint8_t kos_target_test_syscall(uint8_t number, uint8_t bank, uint16_t block)
{
	(void)number;
	(void)bank;
	(void)block;
	return 0u;
}
