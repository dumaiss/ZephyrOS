/* KonaOS CTC0 acceptance helper (test only).
 *
 * The M2/M3/M4 acceptance images arm CTC0 to exercise IRQ delivery and
 * the pending-across-module-load window.  The programming sequence is
 * acceptance-only: it lives in tests/target/support/ctc_test.c and is
 * never linked into a production image.  The production CTC owner keeps
 * only platform_irq_quiesce().
 *
 * Not part of any ABI.
 */

#ifndef KONAOS_TARGET_CTC_TEST_H
#define KONAOS_TARGET_CTC_TEST_H

void kos_ctc0_test_start(void);
void kos_ctc0_test_stop(void);

#endif /* KONAOS_TARGET_CTC_TEST_H */
