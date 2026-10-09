/* ZephyrOS ROMFS-reader acceptance selftest (test artifact).
 *
 * Runs from the late acceptance hook after the current boot path has
 * already loaded the SIO module from the recovery store and bound the
 * console.  It verifies, on target and in the host replay:
 *
 *   1. the boot module is resident and the console is bound;
 *   2. romfs_lookup_ci() finds sio.zmod with the packed size and ZMOD flag;
 *   3. the whole container read through romfs_read() matches the packed
 *      container size and byte sum (byte-identity at the sum level; the
 *      host tests compare SHA-256 against <modules>/sio.zmod);
 *   4. EOF semantics: zero-length read at EOF is clean, one byte past
 *      EOF and beyond-size offsets are rejected;
 *   5. first, last, and cross-48-KiB-window reads of the deterministic
 *      pattern asset return the exact expected bytes.
 *
 * Prints "romfs selftest: OK" through the normal console service on
 * success; a precise FAIL marker with the result code otherwise.
 */

#include "zephyros/romfs.h"
#include "zephyros/module.h"
#include "service_client.h"
#include "zephyros/kernel.h"

#include "romfs_selftest.h"

#if ZOS_SUBSYS_SELFTEST

#include "romfs_expect.inc"

uint8_t romfs_selftest_result;

static uint8_t romfs_buf[64];

/* byte i of the acceptance cross-page pattern asset */
static uint8_t romfs_pattern(uint32_t i)
{
	return (uint8_t)((i & 0xFFu) ^ ((i >> 8) & 0xFFu));
}

static void romfs_put_hex8(uint8_t value)
{
	static const char digits[] = "0123456789ABCDEF";

	service_cli_write(digits + ((value >> 4) & 0x0Fu), 1);
	service_cli_write(digits + (value & 0x0Fu), 1);
}

static void romfs_fail(uint8_t code)
{
	static const char prefix[] = "romfs selftest: FAIL 0x";

	romfs_selftest_result = code;
	service_cli_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	romfs_put_hex8(code);
	service_cli_write("\r\n", 2);
}

/* Read `length` bytes at file-relative `start` and compare them against
 * the generated pattern. */
static uint8_t romfs_check_pattern(const romfs_file_t *file, uint32_t start,
				uint16_t length)
{
	uint16_t i;

	if (romfs_read(file, start, romfs_buf, length) != (int)length) {
		return 0;
	}
	for (i = 0; i < length; ++i) {
		if (romfs_buf[i] != romfs_pattern(start + i)) {
			return 0;
		}
	}
	return 1;
}

void zos_run_romfs_selftest(void)
{
	romfs_file_t sio;
	romfs_file_t romfs_cross;
	uint32_t sum = 0;
	uint32_t offset = 0;
	int result;

	romfs_selftest_result = ZOS_RFST_FAIL_BOOT;

	/* The ROMFS boot must have left a resident module and a bound
	 * console; everything below is relative to that baseline. */
	if (module_count() == 0 ||
	    module_get(0) == (const struct zmod_module *)0 ||
	    service_cli_console_handle() == 0) {
		romfs_fail(ZOS_RFST_FAIL_BOOT);
		return;
	}

	result = romfs_lookup_ci("sio.zmod", &sio);
	if (result != 0 || sio.flags != ROMFS_FLAG_ZMOD) {
		romfs_fail(ZOS_RFST_FAIL_LOOKUP);
		return;
	}
	if (sio.size != SIO_ZMOD_SIZE) {
		romfs_fail(ZOS_RFST_FAIL_SIZE);
		return;
	}

	/* Stream the whole container in bounded chunks; size + byte sum
	 * must match the packed container the loader consumed. */
	while (offset < sio.size) {
		uint16_t chunk = 64;
		uint16_t i;

		if ((uint32_t)chunk > (sio.size - offset)) {
			chunk = (uint16_t)(sio.size - offset);
		}
		result = romfs_read(&sio, offset, romfs_buf, chunk);
		if (result != (int)chunk) {
			romfs_fail(ZOS_RFST_FAIL_SUM);
			return;
		}
		for (i = 0; i < chunk; ++i) {
			sum += romfs_buf[i];
		}
		offset += chunk;
	}
	if ((uint16_t)(sum & 0xFFFFu) != (uint16_t)SIO_ZMOD_SUM) {
		romfs_fail(ZOS_RFST_FAIL_SUM);
		return;
	}

	/* EOF and bound semantics. */
	if (romfs_read(&sio, sio.size, romfs_buf, 0) != 0) {
		romfs_fail(ZOS_RFST_FAIL_EOF);
		return;
	}
	if (romfs_read(&sio, sio.size, romfs_buf, 1) != -ZOS_EINVAL ||
	    romfs_read(&sio, sio.size + 1u, romfs_buf, 0) != -ZOS_EINVAL) {
		romfs_fail(ZOS_RFST_FAIL_EOF);
		return;
	}

	/* First/last/cross-page reads of the deterministic asset. */
	result = romfs_lookup_ci(ZOS_RFST_CROSSPAGE_NAME, &romfs_cross);
	if (result != 0 || romfs_cross.size != ZOS_RFST_CROSSPAGE_SIZE) {
		romfs_fail(ZOS_RFST_FAIL_CROSS);
		return;
	}
	if (!romfs_check_pattern(&romfs_cross, 0, 16)) {
		romfs_fail(ZOS_RFST_FAIL_FIRST);
		return;
	}
	if (!romfs_check_pattern(&romfs_cross, (uint32_t)(romfs_cross.size - 16u), 16)) {
		romfs_fail(ZOS_RFST_FAIL_LAST);
		return;
	}
	{
		/* 16 bytes before and after the 48-KiB logical boundary. */
		uint32_t at = (uint32_t)ZOS_RFST_WINDOW - 16u -
			      (uint32_t)CROSSPAGE_OFFSET;

		if (!romfs_check_pattern(&romfs_cross, at, 32)) {
			romfs_fail(ZOS_RFST_FAIL_CROSS);
			return;
		}
	}

	romfs_selftest_result = ZOS_RFST_OK;
	{
		static const char ok[] = "romfs selftest: OK\r\n";

		if (service_cli_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			romfs_selftest_result = ZOS_RFST_FAIL_CONSOLE;
		}
	}
}

#endif /* ZOS_SUBSYS_SELFTEST */
