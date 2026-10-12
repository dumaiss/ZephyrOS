/* ZephyrOS CTC0 acceptance helper implementation (test only).
 *
 * The exact programming sequence the M2/M3/M4 acceptance images require:
 * vector base, then control word, then time constant to start counting;
 * a control-word reset stops it.  This is the same sequence the retired
 * production helper used, moved verbatim into the test tree so no
 * acceptance-only CTC code is linked into production.
 */

#include "hardware.h"
#include "irq.h"

#include "ctc_test.h"

static __sfr __at ZOS_CTC0_PORT zos_ctc0_test_port;

void zos_ctc0_test_start(void)
{
	zos_ctc0_test_port = ZOS_CTC0_VECTOR_BASE;
	zos_ctc0_test_port = ZOS_CTC0_CONTROL_TEST;
	zos_ctc0_test_port = ZOS_CTC0_TIME_CONSTANT;
}

void zos_ctc0_test_stop(void)
{
	zos_ctc0_test_port = ZOS_CTC0_RESET;
}
