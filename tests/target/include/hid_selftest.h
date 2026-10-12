/* KonaOS HID acceptance diagnostic contract (test artifact).
 *
 * The package-41 diagnostic in tests/target/hid/hid_test.c is a bank-7
 * client of the resident HID driver (see
 * tests/target/include/driver_client.h).  The kernel calls
 * kos_run_hid_selftest() from the late boot hook, after the driver
 * loader placed the image's `sio ioc hid console` set, and the diagnostic pins
 * the HID endpoint at the protocol version, its IOC dependency, the
 * KHID_OP_READY probe, the scripted typed FIFO pass, the console
 * driver's HID input direction,
 * malformed-reply atomicity and the malformed-call refusals.
 *
 * Its last transcript line is "hid selftest: OK" or
 * "hid selftest: FAIL <phase>".  `ace_hid_selftest_result` records the
 * outcome (0 = success, nonzero = the first failing phase's code).
 */

#ifndef KONAOS_TARGET_HID_SELFTEST_H
#define KONAOS_TARGET_HID_SELFTEST_H

extern uint8_t ace_hid_selftest_result;

void kos_run_hid_selftest(void);

#endif /* KONAOS_TARGET_HID_SELFTEST_H */
