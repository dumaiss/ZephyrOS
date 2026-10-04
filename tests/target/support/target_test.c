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
 * (relocated pre-M8 mechanism selftests), the M8 device/console images,
 * or the M9 VFS images.
 */

#include "zephyros/kernel.h"

#if ZOS_SUBSYS_SELFTEST
#include "syscall_selftest.h"
#include "irq_selftest.h"
#include "romfs_selftest.h"
#include "process_selftest.h"
#include "loader_selftest.h"
#endif
#if ZOS_M8_IOC_SELFTEST
#include "m8_ioc_selftest.h"
#endif
#if ZOS_M8_HID_SELFTEST || ZOS_M8_HID_NOIOC || ZOS_M8_HID_BADIOC
#include "m8_hid_selftest.h"
#endif
#if ZOS_M8_V9958_SELFTEST || ZOS_M8_V9958_FONTFAIL
#include "m8_v9958_selftest.h"
#endif
#if ZOS_M8_CONSOLE_SELFTEST
#include "m8_console_selftest.h"
#endif
#if ZOS_M8_SELFTEST
#include "m8_selftest.h"
#endif
#if ZOS_M8_FALLBACK_SELFTEST
#include "m8_fallback_selftest.h"
#endif
#if ZOS_M9_VFS_SELFTEST
#include "m9_selftest.h"
#endif

void zos_target_test_early(void)
{
#if ZOS_SUBSYS_SELFTEST
	zos_run_syscall_selftest();
	zos_run_irq_selftest();
#endif
}

void zos_target_test_late(void)
{
#if ZOS_SUBSYS_SELFTEST
	/* ROMFS reader, then the process/ZEX lifecycle, then the loader: the
	 * loader's fixtures stay resident and hold arena for the rest of the
	 * boot, so that group runs last. */
	zos_run_romfs_selftest();
	zos_run_process_selftest();
	zos_run_loader_selftest();
#endif
#if ZOS_M8_IOC_SELFTEST
	zos_run_m8_ioc_selftest();
#endif
#if ZOS_M8_HID_SELFTEST || ZOS_M8_HID_NOIOC || ZOS_M8_HID_BADIOC
	zos_run_m8_hid_selftest();
#endif
#if ZOS_M8_V9958_SELFTEST || ZOS_M8_V9958_FONTFAIL
	zos_run_m8_v9958_selftest();
#endif
#if ZOS_M8_CONSOLE_SELFTEST
	zos_run_m8_console_selftest();
#endif
#if ZOS_M8_SELFTEST
	zos_run_m8_selftest();
#endif
#if ZOS_M8_FALLBACK_SELFTEST
	zos_run_m8_fallback_selftest();
#endif
#if ZOS_M9_VFS_SELFTEST
	zos_run_m9_vfs_selftest();
#endif
}
