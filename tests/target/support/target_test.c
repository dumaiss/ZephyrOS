/* ZephyrOS target acceptance runner (test artifact).
 *
 * The single integration seam between the production kernel and the
 * acceptance tests.  kernel/main.c calls the two hooks below only when
 * ZOS_TARGET_TEST_BUILD is defined; the concrete acceptance-group
 * selection lives here so production files never contain self-test
 * implementations (docs/architecture.md section 3).
 *
 * Boot phases:
 *   early  before the boot module load (syscall gate, IRQ executive)
 *   late   after console binding (ROMFS reader, process/ZEX, loader)
 *
 * Each acceptance image selects exactly one group: the subsystem image
 * (relocated subsystem mechanism selftests), the device/console subsystem images,
 * or the M9 VFS images.
 */

#include "zephyros/kernel.h"

#if ZOS_SUBSYS_SELFTEST
#include "syscall_selftest.h"
#include "irq_selftest.h"
#include "romfs_selftest.h"
#include "process_selftest.h"
#endif
#if ZOS_IOC_SELFTEST
#include "ioc_selftest.h"
#endif
#if ZOS_HID_SELFTEST
#include "hid_selftest.h"
#endif
#if ZOS_VIDEO_SELFTEST
#include "video_selftest.h"
#endif
#if ZOS_CONSOLE_SELFTEST
#include "console_selftest.h"
#endif
#if ZOS_CONSOLE_INTEGRATION_SELFTEST
#include "console_integration_selftest.h"
#endif
#if ZOS_CONSOLE_FALLBACK_SELFTEST
#include "console_fallback_selftest.h"
#endif
#if ZOS_M9_EXEC_SELFTEST
#include "m9_exec_selftest.h"
#endif
#if ZOS_M9_SELFTEST
#include "m9_integration_selftest.h"
#endif
#if ZOS_NUCLEUS_SELFTEST
#include "nucleus_selftest.h"
#endif
#if ZOS_IPC_SELFTEST
#include "zephyros/service.h"
#include "zos/ipc.h"
#endif

/* Boot-time acceptance modules, called between the production boot set and
 * the normal mount pass.  Only an image that needs extra resident providers
 * defines anything here; every other acceptance image is a no-op (package
 * 42b removed the arena module loader, so no image loads one). */
void zos_boot_test_modules(void)
{
}

void zos_target_test_early(void)
{
#if ZOS_SUBSYS_SELFTEST
	zos_run_syscall_selftest();
	zos_run_irq_selftest();
#endif
#if ZOS_NUCLEUS_SELFTEST
	zos_run_nucleus_selftest();
#endif
}

void zos_target_test_late(void)
{
#if ZOS_SUBSYS_SELFTEST
	/* ROMFS reader, then the process/ZEX lifecycle.  The module-loader
	 * group is retired with the arena (work package 42b): IOC-FAT is a
	 * bank-6 service and no module is loaded into the driver arena. */
	zos_run_romfs_selftest();
	zos_run_process_selftest();
#endif
#if ZOS_IOC_SELFTEST
	zos_run_ioc_selftest();
#endif
#if ZOS_HID_SELFTEST
	zos_run_hid_selftest();
#endif
#if ZOS_VIDEO_SELFTEST
	zos_run_video_selftest();
#endif
#if ZOS_CONSOLE_SELFTEST
	zos_run_console_selftest();
#endif
#if ZOS_CONSOLE_INTEGRATION_SELFTEST
	zos_run_console_integration_selftest();
#endif
#if ZOS_CONSOLE_FALLBACK_SELFTEST
	zos_run_console_fallback_selftest();
#endif
#if ZOS_M9_EXEC_SELFTEST
	zos_run_m9_exec_selftest();
#endif
#if ZOS_M9_SELFTEST
	zos_run_m9_selftest();
#endif
#if ZOS_IPC_SELFTEST
	/* The six synthetic resident services are real service images in
	 * the recovery store; the nucleus service loader reads the boot
	 * manifest and brings them up in bank 6 before the boot launcher
	 * starts the client program (/SYSTEM/ipccopy.zex).  A load failure
	 * is a boot error: the loader reported it on the early console. */
	if (service_load_all(ZOS_SERVICE_MANIFEST_NAME) != 0) {
		panic("service loader failed");
	}
#endif
}

/* TEST-ONLY HOOK: routes acceptance-only syscall numbers to their bodies.
 * Production links neither these bodies nor this hook (package 42b: the
 * object family is production code, so only an acceptance-only number
 * reaches this hook).  Returns non-zero when handled. */
uint8_t zos_target_test_syscall(uint8_t number, uint8_t bank, uint16_t block)
{
	(void)number;
	(void)bank;
	(void)block;
	return 0u;
}
