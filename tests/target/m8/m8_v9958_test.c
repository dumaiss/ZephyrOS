/* ZephyrOS M8 V9958 video acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot: the SIO
 * fallback, the IOC transport, the HID keyboard and the V9958 video core
 * (with its ROMFS font) are loaded from the recovery store and the
 * console is bound.  All control and outcome lines go through the direct
 * early SIO so the transcript is independent of any bound console.
 *
 * Normal image coverage:
 *   - `v9958.0` discovered by class plus all seven VIDEO capabilities;
 *     no character interface and no console-output capability yet (the
 *     console output must still be the SIO fallback);
 *   - the class interface is fetched and verified through
 *     device_interface();
 *   - malformed requests are rejected with the exact error before any
 *     I/O (null/zero write payloads, PRESENT/RESET length, unknown
 *     operation, null state);
 *   - ZVIDEO_RESET re-establishes the full display state;
 *   - PALETTE_WRITE programs entry 12 to magenta, INDIRECT_WRITE starts
 *     a white HMMV fill, PRESENT waits for command completion, and
 *     CTRL_WRITE + DATA_WRITE + DATA_BLOCK stream a deterministic
 *     pattern (200-byte block, far above the historical ceiling) into
 *     VRAM.
 *
 * The visible result and the exact expected bytes are documented in
 * docs/validation/2026-10-03-m8-v9958.md; tests/test_v9958_sim.py
 * replays this image and checks the VRAM contents and port traces.
 *
 * Font-failure image coverage (ZOS_M8_V9958_FONTFAIL): the ROMFS font
 * entry is a 16-byte stub, so the module must fail and publish nothing.
 * The test proves `v9958.0` is absent while HID and the SIO fallback
 * remain registered and bound.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/kernel.h"
#include "zephyros/video.h"

#include "m8_v9958_selftest.h"

#if ZOS_M8_V9958_SELFTEST || ZOS_M8_V9958_FONTFAIL

uint8_t m8_v9958_selftest_result;

#define M8V9958_VIDEO_CAPS (ZCAP_VIDEO_CTRL_WRITE | ZCAP_VIDEO_DATA_WRITE | \
			    ZCAP_VIDEO_DATA_BLOCK | ZCAP_VIDEO_PALETTE | \
			    ZCAP_VIDEO_INDIRECT | ZCAP_VIDEO_PRESENT | \
			    ZCAP_VIDEO_RESET)

/* Deterministic pattern: one white pixel pair written through
 * DATA_WRITE, then 200 magenta pixel pairs through DATA_BLOCK. */
#define M8V9958_BAND_SCANLINE 96u     /* VRAM byte offset 6000h */
#define M8V9958_BAND_R14      0x01u   /* A15:A14 of 6000h        */
#define M8V9958_BAND_ADDR_HI  0x60u   /* A13:A8 (20h) | VRAM 40h */
#define M8V9958_BAND_BYTES    200u
#define M8V9958_BAND_FIRST    0xFFu   /* white,white             */
#define M8V9958_BAND_FILL     0xCCu   /* palette 12 twice        */

static const char m8v9958_digits[] = "0123456789ABCDEF";

static void m8v9958_put_hex8(uint8_t value)
{
	early_console_putc(m8v9958_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m8v9958_digits[value & 0x0Fu]);
}

static void m8v9958_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "m8 v9958 selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	m8_v9958_selftest_result = code;
	early_console_puts(prefix);
	m8v9958_put_hex8(code);
	early_console_puts(detail_prefix);
	m8v9958_put_hex8(detail);
	early_console_puts("\r\n");
}

static uint8_t m8v9958_name_is(const char *name, const char *want)
{
	while (*want != '\0') {
		if (*name != *want) {
			return 0u;
		}
		++name;
		++want;
	}
	return (*name == '\0') ? 1u : 0u;
}

#if ZOS_M8_V9958_FONTFAIL
/* Resource-failure image: the module must have failed its font check
 * and been rolled back; the earlier devices and the console binding
 * must be untouched. */
static void m8v9958_fontfail_run(void)
{
	if (device_find(ZDEV_VIDEO, ZCAP_VIDEO_RESET, 0u) !=
	    ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_PRESENT, 0x01u);
		return;
	}
	if (device_find(ZDEV_HID, ZCAP_HID_TEXT, 0u) ==
	    ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_DEVICES, 0x01u);
		return;
	}
	if (device_find(ZDEV_SERIAL, ZCAP_CONSOLE_OUT, 0u) ==
	    ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_DEVICES, 0x02u);
		return;
	}
	if (console_output_handle() == ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_DEVICES, 0x03u);
		return;
	}
	early_console_puts("m8 v9958: absent after font failure\r\n");
	m8_v9958_selftest_result = ZOS_M8V9958_OK;
	early_console_puts("m8 v9958 fontfail selftest: OK\r\n");
}
#else
static void m8v9958_run(void)
{
	zdev_handle_t handle;
	const struct zdevice *dev;
	const void *class_ops = (const void *)0;
	void *state = (void *)0;
	const struct zvideo_ops *ops;
	zdev_handle_t sio;
	uint8_t cmd[15];
	uint8_t bytes[2];
	uint8_t pattern[M8V9958_BAND_BYTES];
	uint8_t i;
	int rc;

	handle = device_find(ZDEV_VIDEO, M8V9958_VIDEO_CAPS, 0u);
	if (handle == ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_BOOT, 0x01u);
		return;
	}
	dev = device_get(handle);
	if (dev == (const struct zdevice *)0) {
		m8v9958_fail(ZOS_M8V9958_FAIL_BOOT, 0x02u);
		return;
	}
	if (dev->class_id != ZDEV_VIDEO) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x01u);
		return;
	}
	if (m8v9958_name_is(dev->name, "v9958.0") == 0u) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x02u);
		return;
	}
	if (dev->capabilities != M8V9958_VIDEO_CAPS) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x03u);
		return;
	}
	if (dev->char_ops != (const struct zchar_ops *)0) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x04u);
		return;
	}
	if (device_find(ZDEV_VIDEO, ZCAP_CONSOLE_OUT, 0u) !=
	    ZDEV_HANDLE_INVALID) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x05u);
		return;
	}
	if (device_interface(handle, ZDEV_VIDEO, &class_ops, &state) !=
	    ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x06u);
		return;
	}
	ops = (const struct zvideo_ops *)class_ops;
	if (ops == (const struct zvideo_ops *)0 || ops->send == 0 ||
	    state == (void *)0) {
		m8v9958_fail(ZOS_M8V9958_FAIL_SHAPE, 0x07u);
		return;
	}
	early_console_puts("m8 v9958: video0 OK\r\n");

	/* Malformed requests: exact errors, no I/O performed. */
	rc = ops->send(state, ZVIDEO_PRESENT, (const void *)0, 5u);
	if (rc != ZDEV_EINVAL) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x01u);
		return;
	}
	rc = ops->send(state, ZVIDEO_RESET, (const void *)0, 1u);
	if (rc != ZDEV_EINVAL) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x02u);
		return;
	}
	rc = ops->send(state, ZVIDEO_DATA_BLOCK, (const void *)0, 64u);
	if (rc != ZDEV_EINVAL) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x03u);
		return;
	}
	rc = ops->send(state, ZVIDEO_DATA_BLOCK, pattern, 0u);
	if (rc != ZDEV_EINVAL) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x04u);
		return;
	}
	rc = ops->send(state, 0x40u, (const void *)0, 0u);
	if (rc != ZDEV_ENOTSUP) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x05u);
		return;
	}
	rc = ops->send((void *)0, ZVIDEO_PRESENT, (const void *)0, 0u);
	if (rc != ZDEV_ENODEV) {
		m8v9958_fail(ZOS_M8V9958_FAIL_MALFORMED, 0x06u);
		return;
	}
	early_console_puts("m8 v9958: malformed rejected\r\n");

	/* Reset re-establishes every owned register and VRAM asset. */
	if (ops->send(state, ZVIDEO_RESET, (const void *)0, 0u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x01u);
		return;
	}
	early_console_puts("m8 v9958: reset OK\r\n");

	/* The console output must still be the SIO fallback: this stage has
	 * no character/console-output interface on the VIDEO device. */
	sio = device_find(ZDEV_SERIAL, ZCAP_CONSOLE_OUT, 0u);
	if (sio == ZDEV_HANDLE_INVALID ||
	    console_output_handle() != sio) {
		m8v9958_fail(ZOS_M8V9958_FAIL_BIND, 0x01u);
		return;
	}

	/* Palette entry 12 -> magenta: RB=77h (R=7,B=7), G=00h. */
	bytes[0] = 0x0Cu;
	bytes[1] = 0x90u; /* R#16 | register select */
	if (ops->send(state, ZVIDEO_CTRL_WRITE, bytes, 2u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x02u);
		return;
	}
	bytes[0] = 0x77u;
	bytes[1] = 0x00u;
	if (ops->send(state, ZVIDEO_PALETTE_WRITE, bytes, 2u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x03u);
		return;
	}

	/* White 64x64 fill at (64,32): R#17 = 32 then the 15-byte HMMV
	 * block through the indirect port; PRESENT waits for completion. */
	bytes[0] = 32u;
	bytes[1] = 0x91u; /* R#17 | register select */
	if (ops->send(state, ZVIDEO_CTRL_WRITE, bytes, 2u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x04u);
		return;
	}
	for (i = 0u; i < 15u; ++i) {
		cmd[i] = 0x00u;
	}
	cmd[4] = 0x40u;  /* DX low = 64  */
	cmd[6] = 0x20u;  /* DY low = 32  */
	cmd[8] = 0x40u;  /* NX low = 64  */
	cmd[10] = 0x20u; /* NY low = 32  */
	cmd[12] = 0xFFu; /* color white  */
	cmd[14] = 0xC0u; /* HMMV         */
	if (ops->send(state, ZVIDEO_INDIRECT_WRITE, cmd, 15u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x05u);
		return;
	}
	if (ops->send(state, ZVIDEO_PRESENT, (const void *)0, 0u) !=
	    ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x06u);
		return;
	}
	early_console_puts("m8 v9958: fill presented\r\n");

	/* Visible band at scanline 96 (VRAM 6000h): one white byte through
	 * DATA_WRITE, then a 200-byte magenta block through DATA_BLOCK. */
	bytes[0] = M8V9958_BAND_R14;
	bytes[1] = 0x8Eu; /* R#14 | register select */
	if (ops->send(state, ZVIDEO_CTRL_WRITE, bytes, 2u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x07u);
		return;
	}
	bytes[0] = 0x00u;
	bytes[1] = M8V9958_BAND_ADDR_HI;
	if (ops->send(state, ZVIDEO_CTRL_WRITE, bytes, 2u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x08u);
		return;
	}
	bytes[0] = M8V9958_BAND_FIRST;
	if (ops->send(state, ZVIDEO_DATA_WRITE, bytes, 1u) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x09u);
		return;
	}
	for (i = 0u; i < M8V9958_BAND_BYTES; ++i) {
		pattern[i] = M8V9958_BAND_FILL;
	}
	if (ops->send(state, ZVIDEO_DATA_BLOCK, pattern,
		      M8V9958_BAND_BYTES) != ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x0Au);
		return;
	}
	if (ops->send(state, ZVIDEO_PRESENT, (const void *)0, 0u) !=
	    ZDEV_OK) {
		m8v9958_fail(ZOS_M8V9958_FAIL_OPS, 0x0Bu);
		return;
	}
	(void)M8V9958_BAND_SCANLINE;
	early_console_puts("m8 v9958: pattern drawn\r\n");

	m8_v9958_selftest_result = ZOS_M8V9958_OK;
	early_console_puts("m8 v9958 selftest: OK\r\n");
}
#endif /* ZOS_M8_V9958_FONTFAIL */

void zos_run_m8_v9958_selftest(void)
{
	m8_v9958_selftest_result = ZOS_M8V9958_FAIL_BOOT;
#if ZOS_M8_V9958_FONTFAIL
	m8v9958_fontfail_run();
#else
	m8v9958_run();
#endif
}

#endif /* ZOS_M8_V9958_SELFTEST || ZOS_M8_V9958_FONTFAIL */
