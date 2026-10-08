/* ZephyrOS IPC acceptance loader seam (test artifact).
 *
 * The ipc-selftest image loads the six synthetic bank-6 services before
 * the client boots; the loader lives in the test tree
 * (tests/target/ipc/ipc_loader.c) and is invoked from the single
 * documented test-build hook (zos_target_test_late, ZOS_IPC_SELFTEST).
 */

#ifndef ZEPHYROS_TARGET_IPC_SELFTEST_H
#define ZEPHYROS_TARGET_IPC_SELFTEST_H

/* Validate, place, relocate and start the six synthetic services in
 * bank 6.  Returns 0 when all six are registered; a non-zero code names
 * the failed step of the first failing container (the loader prints the
 * container name). */
int zos_ipc_services_load(void);

#endif /* ZEPHYROS_TARGET_IPC_SELFTEST_H */
