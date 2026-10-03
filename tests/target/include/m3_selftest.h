/* ZephyrOS M3 acceptance constants (test only).
 *
 * Shared by tests/target/m3/m3_test.c (target) and the host replay in
 * tests/test_m3_sim.py; the host test parses this header so the two
 * cannot drift.  Not part of the native ABI.
 */

#ifndef ZEPHYROS_TARGET_M3_SELFTEST_H
#define ZEPHYROS_TARGET_M3_SELFTEST_H

/* Result codes recorded in m3_selftest_result (0 = success). */
#define ZOS_M3ST_OK              0x00u
#define ZOS_M3ST_FAIL_REGISTRY   0x01u
#define ZOS_M3ST_FAIL_CAPABILITY 0x02u
#define ZOS_M3ST_FAIL_DUPLICATE  0x03u
#define ZOS_M3ST_FAIL_INVALID    0x04u
#define ZOS_M3ST_FAIL_ROLLBACK   0x05u
#define ZOS_M3ST_FAIL_EXHAUSTION 0x06u
#define ZOS_M3ST_FAIL_CONSOLE    0x07u
#define ZOS_M3ST_FAIL_TX         0x08u
#define ZOS_M3ST_FAIL_RX         0x09u
#define ZOS_M3ST_FAIL_RX_BYTE    0x0Au
#define ZOS_M3ST_FAIL_IRQ        0x0Bu

/* Byte the operator/smoke script sends when the RX test announces
 * readiness ('R'). */
#define ZOS_M3ST_RX_BYTE 0x52u

/* Bound on console RX polling attempts (each attempt includes a short
 * pacing delay, so the window spans seconds on 10 MHz hardware). */
#define ZOS_M3ST_RX_ATTEMPTS 0x0600u

#if ZOS_M3_SELFTEST
/* M3 acceptance entry (tests/target/m3/m3_test.c). */
void zos_run_m3_selftest(void);
#endif

#endif /* ZEPHYROS_TARGET_M3_SELFTEST_H */
