/* ZephyrOS M7 diagnostic acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the production-style boot (SIO
 * loaded from ROMFS, normal console bound) and validates the SYS_DIAG_*
 * boundary directly through the bank-7 dispatcher:
 *
 *   - BANK_INFO returns mask 7Fh | bank in range;
 *   - SYSTEM_INFO writes the exact eight-byte v1 record;
 *   - MEM_WRITE/MEM_READ round-trips a byte in a free application bank;
 *   - page-zero reads are allowed; bank 7, out-of-range addresses, the
 *     reserved field, latch-aliased ports, and out-of-window blocks are
 *     rejected with ZOS_EINVAL;
 *   - rejected requests are side-effect free (canary intact, exact latch
 *     restored, process table unchanged);
 *   - an accepted port input works through the diagnostic boundary.
 *
 * The result line is printed through the normal console; the interactive
 * acceptance continues afterwards in the temporary launcher.
 */

#include "zephyros/process.h"
#include "zephyros/syscall.h"
#include "zephyros/console.h"
#include "zephyros/module.h"
#include "zephyros/kernel.h"
#include "zephyros/platform.h"

#include "m7_selftest.h"

#if ZOS_M7_SELFTEST

uint8_t m7_selftest_result;

/* Bank-7 dispatcher internals, entered exactly as the asm boundary
 * enters them (test artifact: this is the same entry the RST gate
 * uses). */
extern uint8_t zos_syscall_number;
extern uint16_t zos_syscall_args;
extern uint8_t zos_syscall_result_a;
extern uint8_t zos_syscall_result_c;
extern uint16_t zos_syscall_result_hl;
extern void kernel_syscall_dispatch(void);

/* Scratch argument block in the caller window 0100h-1FFFh; in kernel
 * context (mode 11 over bank 0) that is bank-0 low RAM. */
#define M7_REQ_BLOCK 0x1F00u
#define M7_REQ ((volatile uint8_t *)M7_REQ_BLOCK)

#define M7_EINVAL 2u

static const char digits[] = "0123456789ABCDEF";

static void m7_put_hex8(uint8_t value)
{
	console_write(digits + ((value >> 4) & 0x0Fu), 1);
	console_write(digits + (value & 0x0Fu), 1);
}

static void m7_put_hex16(uint16_t value)
{
	m7_put_hex8((uint8_t)(value >> 8));
	m7_put_hex8((uint8_t)(value & 0xFFu));
}

static void m7_fail(uint8_t code, uint8_t check, uint16_t detail)
{
	static const char prefix[] = "m7 selftest: FAIL 0x";
	static const char mid[] = " check 0x";
	static const char detail_prefix[] = " detail 0x";

	m7_selftest_result = code;
	console_write(prefix, (uint16_t)(sizeof(prefix) - 1u));
	m7_put_hex8(code);
	console_write(mid, (uint16_t)(sizeof(mid) - 1u));
	m7_put_hex8(check);
	console_write(detail_prefix, (uint16_t)(sizeof(detail_prefix) - 1u));
	m7_put_hex16(detail);
	console_write("\r\n", 2);
}

/* Invoke one diagnostic service through the real dispatcher.  Returns
 * 0 on success, the error code on failure; *result receives HL. */
static uint8_t m7_call(uint8_t number, uint16_t block, uint16_t *result)
{
	zos_syscall_number = number;
	zos_syscall_args = block;
	zos_syscall_result_hl = 0xDEADu;
	zos_syscall_result_a = 0xEEu;
	zos_syscall_result_c = 0xEEu;
	kernel_syscall_dispatch();
	if (result != (uint16_t *)0) {
		*result = zos_syscall_result_hl;
	}
	return (uint8_t)(zos_syscall_result_c != 0 ? zos_syscall_result_a : 0);
}

static uint16_t m7_mem_read_req(uint8_t bank, uint8_t reserved,
				uint16_t address)
{
	M7_REQ[0] = bank;
	M7_REQ[1] = reserved;
	M7_REQ[2] = (uint8_t)(address & 0xFFu);
	M7_REQ[3] = (uint8_t)(address >> 8);
	return M7_REQ_BLOCK;
}

static uint16_t m7_mem_write_req(uint8_t bank, uint8_t value,
				 uint16_t address)
{
	M7_REQ[0] = bank;
	M7_REQ[1] = value;
	M7_REQ[2] = (uint8_t)(address & 0xFFu);
	M7_REQ[3] = (uint8_t)(address >> 8);
	return M7_REQ_BLOCK;
}

static uint8_t m7_mem_read(uint8_t bank, uint8_t reserved, uint16_t address,
			   uint16_t *value)
{
	return m7_call(ZOS_SYSCALL_DIAG_MEM_READ,
		       m7_mem_read_req(bank, reserved, address), value);
}

static uint8_t m7_mem_write(uint8_t bank, uint8_t value, uint16_t address,
			    uint16_t *result)
{
	return m7_call(ZOS_SYSCALL_DIAG_MEM_WRITE,
		       m7_mem_write_req(bank, value, address), result);
}

static uint8_t m7_io_in(uint8_t port, uint16_t *value)
{
	M7_REQ[0] = port;
	return m7_call(ZOS_SYSCALL_DIAG_IO_IN, M7_REQ_BLOCK, value);
}

static uint8_t m7_io_out(uint8_t port, uint8_t value)
{
	M7_REQ[0] = port;
	M7_REQ[1] = value;
	return m7_call(ZOS_SYSCALL_DIAG_IO_OUT, M7_REQ_BLOCK,
		       (uint16_t *)0);
}

void zos_run_m7_selftest(void)
{
	uint16_t result;
	uint16_t value;
	uint8_t error;
	uint8_t latch0;

	m7_selftest_result = ZOS_M7ST_FAIL_BOOT;
	if (module_count() == 0 ||
	    module_get(0) == (const struct zmod_module *)0 ||
	    console_output_handle() == 0 ||
	    process_active_count() != 0 ||
	    process_free_banks() != PROCESS_BANK_COUNT) {
		m7_fail(ZOS_M7ST_FAIL_BOOT, 0, 0);
		return;
	}
	latch0 = platform_memctl_read();

	/* 1. BANK_INFO: mask 7Fh, current bank 0-6, latch restored. */
	error = m7_call(ZOS_SYSCALL_DIAG_BANK_INFO, 0, &result);
	if (error != 0 || (result >> 8) != ZOS_DIAG_BANK_MASK ||
	    (result & 0xFFu) > PROCESS_MAX_BANK) {
		m7_fail(ZOS_M7ST_FAIL_BANK, ZOS_M7ST_CHECK_BANK_INFO, result);
		return;
	}
	if (platform_memctl_read() != latch0) {
		m7_fail(ZOS_M7ST_FAIL_LATCH, ZOS_M7ST_CHECK_BANK_INFO, result);
		return;
	}

	/* 2. SYSTEM_INFO: exact eight-byte v1 record. */
	error = m7_call(ZOS_SYSCALL_DIAG_SYSTEM_INFO, M7_REQ_BLOCK, &result);
	if (error != 0 || result != ZOS_DIAG_SYSTEM_INFO_SIZE ||
	    M7_REQ[0] != ZOS_DIAG_SYSTEM_INFO_SIZE ||
	    M7_REQ[1] != ZOS_DIAG_SYSTEM_INFO_ABI ||
	    M7_REQ[2] != ZOS_MODE_APPLICATION ||
	    M7_REQ[3] != ZOS_MODE_OS ||
	    M7_REQ[4] != (uint8_t)(ZOS_COMMON_BASE & 0xFFu) ||
	    M7_REQ[5] != (uint8_t)(ZOS_COMMON_BASE >> 8) ||
	    M7_REQ[6] != PROCESS_KERNEL_BANK ||
	    M7_REQ[7] != ZOS_ROMFS_FIRST_PAGE) {
		m7_fail(ZOS_M7ST_FAIL_SYSINFO, ZOS_M7ST_CHECK_SYSTEM_INFO,
			result);
		return;
	}

	/* 3. Memory round trip in a free application bank. */
	error = m7_mem_write(1, 0x5Au, 0x0B00u, &result);
	if (error != 0 || result != 0) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_MEM_ROUND, error);
		return;
	}
	error = m7_mem_read(1, 0, 0x0B00u, &value);
	if (error != 0 || value != 0x5Au) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_MEM_ROUND, value);
		return;
	}

	/* 4. Page-zero reads are allowed (bank 0 RST 30h vector). */
	error = m7_mem_read(0, 0, 0x0030u, &value);
	if (error != 0 || value != 0xC3u) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_PAGE_ZERO, value);
		return;
	}

	/* 5. Argument blocks outside 0100h-1FFFh are rejected. */
	error = m7_call(ZOS_SYSCALL_DIAG_MEM_READ, 0x2000u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_BLOCK, error);
		return;
	}
	error = m7_call(ZOS_SYSCALL_DIAG_SYSTEM_INFO, 0x2000u, &result);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_BLOCK, error);
		return;
	}

	/* 6. Bank 7 (and out-of-range bank bytes) are rejected. */
	error = m7_mem_read(7, 0, 0x0B00u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_BANKS, error);
		return;
	}
	error = m7_mem_read(0xFFu, 0, 0x0B00u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_BANKS, error);
		return;
	}
	error = m7_mem_write(7, 0xAAu, 0x0B00u, &result);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_BANKS, error);
		return;
	}

	/* 7. Read range 0000h-DFFFh, write range 0100h-DFFFh. */
	error = m7_mem_read(1, 0, 0xE000u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_RANGES, error);
		return;
	}
	error = m7_mem_read(1, 0, 0xFFFFu, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_RANGES, error);
		return;
	}
	error = m7_mem_write(1, 0xAAu, 0x00FFu, &result);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_RANGES, error);
		return;
	}
	error = m7_mem_write(1, 0xAAu, 0xE000u, &result);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_BAD_RANGES, error);
		return;
	}

	/* 8. The mem-read reserved field must be zero. */
	error = m7_mem_read(1, 1, 0x0B00u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_RESERVED, error);
		return;
	}

	/* 9. Rejected writes are side-effect free: the canary written
	 * before the rejected bank-7 write must read back unchanged. */
	error = m7_mem_write(1, 0x11u, 0x0B01u, &result);
	if (error != 0) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_SIDE_EFFECT, error);
		return;
	}
	error = m7_mem_write(7, 0xAAu, 0x0B01u, &result);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_SIDE_EFFECT,
			error);
		return;
	}
	error = m7_mem_read(1, 0, 0x0B01u, &value);
	if (error != 0 || value != 0x11u) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_SIDE_EFFECT, value);
		return;
	}

	/* 10. Harmless port input through the diagnostic boundary. */
	error = m7_io_in(0x20u, &value);
	if (error != 0 || value > 0xFFu) {
		m7_fail(ZOS_M7ST_FAIL_MEM, ZOS_M7ST_CHECK_IO_GOOD, error);
		return;
	}

	/* 11. Latch-aliased ports are rejected for both directions. */
	error = m7_io_in(0x00u, &value);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_IO_LATCH, error);
		return;
	}
	error = m7_io_out(0x00u, 0x18u);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_IO_LATCH, error);
		return;
	}
	error = m7_io_out(0x0Fu, 0x18u);
	if (error != M7_EINVAL) {
		m7_fail(ZOS_M7ST_FAIL_REJECT, ZOS_M7ST_CHECK_IO_LATCH, error);
		return;
	}

	/* 12. Exact latch and process bookkeeping stability. */
	if (platform_memctl_read() != latch0) {
		m7_fail(ZOS_M7ST_FAIL_LATCH, ZOS_M7ST_CHECK_LATCH_STABLE, 0);
		return;
	}
	if (process_active_count() != 0 ||
	    process_free_banks() != PROCESS_BANK_COUNT ||
	    process_current_pid() != PROCESS_NO_PROCESS) {
		m7_fail(ZOS_M7ST_FAIL_TABLE, ZOS_M7ST_CHECK_LATCH_STABLE, 0);
		return;
	}

	m7_selftest_result = ZOS_M7ST_OK;
	{
		static const char ok[] = "m7 selftest: OK\r\n";

		if (console_write(ok, (uint16_t)(sizeof(ok) - 1u)) !=
		    (int)(sizeof(ok) - 1u)) {
			m7_selftest_result = ZOS_M7ST_FAIL_CONSOLE;
		}
	}
}

#endif /* ZOS_M7_SELFTEST */
