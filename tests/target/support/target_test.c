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
 *   late   after console binding (M3 device layer, M4 loader/dynamic)
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
}
