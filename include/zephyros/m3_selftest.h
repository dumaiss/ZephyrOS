/* ZephyrOS M3 target self-test constants (build option only).
 *
 * Shared by kernel/device/m3_selftest.c (target) and the host replay in
 * tests/test_m3_sim.py; the host test parses this header so the two
 * cannot drift.  Test-only; not part of the native ABI.
 */

#ifndef ZEPHYROS_M3_SELFTEST_H
#define ZEPHYROS_M3_SELFTEST_H

/* Result codes recorded in m3_selftest_result (0 = success). */
#define ZOS_M3ST_OK              0x00u
#define ZOS_M3ST_FAIL_REGISTRY   0x01u
#define ZOS_M3ST_FAIL_CAPABILITY 0x02u
#define ZOS_M3ST_FAIL_DUPLICATE  0x03u
#define ZOS_M3ST_FAIL_INVALID    0x04u
#define ZOS_M3ST_FAIL_DRIVERS    0x05u
#define ZOS_M3ST_FAIL_ROLLBACK   0x06u
#define ZOS_M3ST_FAIL_EXHAUSTION 0x07u
#define ZOS_M3ST_FAIL_CONSOLE    0x08u
#define ZOS_M3ST_FAIL_TX         0x09u
#define ZOS_M3ST_FAIL_RX         0x0Au
#define ZOS_M3ST_FAIL_RX_BYTE    0x0Bu
#define ZOS_M3ST_FAIL_IRQ        0x0Cu

/* Byte the operator/smoke script sends when the RX test announces
 * readiness ('R'). */
#define ZOS_M3ST_RX_BYTE 0x52u

/* Bound on console RX polling attempts (each attempt includes a short
 * pacing delay, so the window spans seconds on 10 MHz hardware). */
#define ZOS_M3ST_RX_ATTEMPTS 0x0600u

#if ZOS_M3_SELFTEST
/* Target-side self-test entry (kernel/device/m3_selftest.c). */
void zos_run_m3_selftest(void);
#endif

#endif /* ZEPHYROS_M3_SELFTEST_H */
