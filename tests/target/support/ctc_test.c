/* KonaOS CTC0 acceptance helper implementation (test only).
 *
 * The exact programming sequence the IRQ acceptance needs: vector base,
 * then control word, then time constant to start counting; a
 * control-word reset stops it.  It lives in the test tree so no
 * acceptance-only CTC code is linked into production.
 */

#include "hardware.h"
#include "irq.h"

#include "ctc_test.h"

static __sfr __at KOS_CTC0_PORT kos_ctc0_test_port;

void kos_ctc0_test_start(void)
{
	kos_ctc0_test_port = KOS_CTC0_VECTOR_BASE;
	kos_ctc0_test_port = KOS_CTC0_CONTROL_TEST;
	kos_ctc0_test_port = KOS_CTC0_TIME_CONSTANT;
}

void kos_ctc0_test_stop(void)
{
	kos_ctc0_test_port = KOS_CTC0_RESET;
}
