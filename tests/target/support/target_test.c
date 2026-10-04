/* ZephyrOS target acceptance runner (test artifact).
 *
 * The single integration seam between the production kernel and the
 * milestone acceptance tests.  kernel/main.c calls the two hooks below
 * only when ZOS_TARGET_TEST_BUILD is defined; the concrete milestone
 * selection lives here so production files never contain self-test
 * implementations (docs/architecture.md section 3).
 *
 * Boot phases:
 *   early  before the boot catalog load (M1 syscall gate, M2 IRQ)
 *   late   after console binding (M3 device layer, M4 loader/dynamic,
 *          M5 ROMFS)
 */

#include "zephyros/kernel.h"

#if ZOS_SYSCALL_SELFTEST
#include "selftest.h"
#endif
#if ZOS_IRQ_SELFTEST
#include "irq_selftest.h"
#endif
#if ZOS_M3_SELFTEST
#include "m3_selftest.h"
#endif
#if ZOS_M4_SELFTEST || ZOS_M4_FAILURE_SELFTEST
#include "m4_selftest.h"
#endif
#if ZOS_M5_SELFTEST
#include "m5_selftest.h"
#endif
#if ZOS_M6_SELFTEST || ZOS_M6_FAILURE_SELFTEST
#include "m6_selftest.h"
#endif
#if ZOS_M7_SELFTEST
#include "m7_selftest.h"
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

void zos_target_test_early(void)
{
#if ZOS_SYSCALL_SELFTEST
	zos_run_syscall_selftest();
#endif
#if ZOS_IRQ_SELFTEST
	zos_run_irq_selftest();
#endif
}

void zos_target_test_late(void)
{
#if ZOS_M3_SELFTEST
	zos_run_m3_selftest();
#endif
#if ZOS_M4_SELFTEST
	zos_run_m4_selftest();
#endif
#if ZOS_M4_FAILURE_SELFTEST
	zos_run_m4_failure_selftest();
#endif
#if ZOS_M5_SELFTEST
	zos_run_m5_selftest();
#endif
#if ZOS_M6_SELFTEST
	zos_run_m6_selftest();
#endif
#if ZOS_M6_FAILURE_SELFTEST
	zos_run_m6_failure_selftest();
#endif
#if ZOS_M7_SELFTEST
	zos_run_m7_selftest();
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
}
