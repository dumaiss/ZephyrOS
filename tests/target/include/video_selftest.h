/* ZephyrOS V9958 video driver acceptance selftest contract (test
 * artifact, work package 41).
 *
 * The nucleus acceptance runner (tests/target/support/target_test.c)
 * calls zos_run_video_selftest() from the late boot hook, after the
 * resident driver loader placed and started this image's driver set
 * (`sio v9958 console`) and after the nucleus console probe wrote
 * through the console driver.  The diagnostic is a bank-7 client of
 * the video driver: it drives the descriptor-less protocol forms (see
 * tests/target/include/driver_client.h), records its transcript on the
 * early SIO console, and its last line is `video selftest: OK` or
 * `video selftest: FAIL <phase>`.
 *
 * The coverage and the replay's assertions are documented in the
 * diagnostic itself (tests/target/video/v9958_test.c) and in
 * tests/test_v9958_sim.py, which replays this image against the V9958
 * peer and pins the transcript plus the peer-observable facts.
 */

#ifndef ZEPHYROS_TARGET_VIDEO_SELFTEST_H
#define ZEPHYROS_TARGET_VIDEO_SELFTEST_H

/* Result codes recorded in ace_video_selftest_result (0 = success). */
#define ZOS_VIDEOSELFTEST_OK             0x00u
#define ZOS_VIDEOSELFTEST_FAIL_ENDPOINT  0x01u /* no VIDEO/serial/console ep */
#define ZOS_VIDEOSELFTEST_FAIL_PRESENT   0x02u /* PRESENT refused           */
#define ZOS_VIDEOSELFTEST_FAIL_RESET     0x03u /* RESET refused             */
#define ZOS_VIDEOSELFTEST_FAIL_PUTC      0x04u /* marker PUTC refused       */
#define ZOS_VIDEOSELFTEST_FAIL_MALFORMED 0x05u /* malformed call accepted   */

/* Recorded outcome for the host replay / smoke transcript. */
extern uint8_t ace_video_selftest_result;

void zos_run_video_selftest(void);

#endif /* ZEPHYROS_TARGET_VIDEO_SELFTEST_H */
