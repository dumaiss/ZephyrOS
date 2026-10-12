/* ZephyrOS process acceptance program: zextest.
 *
 * Built as tests/subsys/zextest.zex (user/sdk/src/crt0.asm
 * + this file + user/sdk/src/console.asm), stored in the subsystem
 * recovery image, and executed by the kernel as the child of the process
 * acceptance test.  It verifies, on target and in the host replay:
 *
 *   1. the BSS region was zeroed by the loader;
 *   2. the constant image (code/RODATA) was copied byte-for-byte
 *      (checked through a known sentinel table);
 *   3. the initial stack lies inside the reserved [E000-stack_size,
 *      E000h) window;
 *   4. SYS_GETINFO returns the six-byte v1 record with this process'
 *      identity and the kernel-parent flag;
 *   5. SYS_CONSOLE_WRITE reaches the normal console device;
 *   6. the process exits with PASS|bank (a known status that also proves
 *      which bank ran the identical ROMFS payload).
 *
 * The program uses the user runtime wrappers only; it never touches
 * kernel symbols or SDCC's calling conventions across the ABI.
 */

#include "zos.h"
#include "process_selftest.h"

/* Stack window: must match the zextest zex_program STACK=256 option. */
#define ZT_STACK_TOP  0xE000u
#define ZT_STACK_SIZE 256u

/* Assembly helpers (tests/target/m6/zextest_entry.asm). */
extern unsigned int zt_get_sp(void);

/* Uninitialized: the loader must zero this whole region. */
static unsigned char bss_probe[32];

/* Initialized constant: must arrive from the packed image intact. */
static const unsigned char ro_sentinel[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

static const char bank_prefix[] = "zextest: bank=";
static const char pid_sep[] = " pid=";
static const char line_end[] = "\r\n";
static const char ok_line[] = "zextest: OK\r\n";
static const char fail_prefix[] = "zextest: FAIL 0x";
static const char hex_digits[] = "0123456789ABCDEF";

static unsigned char scratch_char;

static void zt_fail(unsigned char code)
{
	/* The console may already be broken; the exit status still reports
	 * the failure, so this write result is deliberately ignored. */
	console_write(fail_prefix, sizeof(fail_prefix) - 1);
	scratch_char = hex_digits[(code >> 4) & 0x0Fu];
	console_write(&scratch_char, 1);
	scratch_char = hex_digits[code & 0x0Fu];
	console_write(&scratch_char, 1);
	console_write(line_end, sizeof(line_end) - 1);
	exit(ZOS_PCST_ZEXTEST_FAIL | code);
}

static void zt_write_str(const char *text, unsigned int len)
{
	if (console_write(text, len) != (int)len) {
		zt_fail(ZOS_PCST_ZT_FAIL_WRITE);
	}
}

int main(void)
{
	struct zos_info_record info;
	unsigned char i;
	unsigned int sp;

	/* 1. BSS zero. */
	for (i = 0; i < sizeof(bss_probe); ++i) {
		if (bss_probe[i] != 0) {
			zt_fail(ZOS_PCST_ZT_FAIL_BSS);
		}
	}

	/* 2. Image copy. */
	if (ro_sentinel[0] != 0xDEu || ro_sentinel[1] != 0xADu ||
	    ro_sentinel[2] != 0xBEu || ro_sentinel[3] != 0xEFu) {
		zt_fail(ZOS_PCST_ZT_FAIL_IMAGE);
	}

	/* 3. Stack bounds: SP must be strictly inside the reserved window
	 *    (top exclusive; room for the common return trap and frames). */
	sp = zt_get_sp();
	if (sp >= ZT_STACK_TOP ||
	    sp < (unsigned int)(ZT_STACK_TOP - ZT_STACK_SIZE + 4u)) {
		zt_fail(ZOS_PCST_ZT_FAIL_STACK);
	}

	/* 4. GETINFO record. */
	if (getinfo(&info) != ZOS_GETINFO_SIZE) {
		zt_fail(ZOS_PCST_ZT_FAIL_GETINFO);
	}
	if (info.size != ZOS_GETINFO_SIZE || info.abi != ZOS_GETINFO_ABI ||
	    info.bank > 6u || info.pid > 6u ||
	    (info.flags & ZOS_PROCESS_FLAG_KERNEL_PARENT) == 0u) {
		zt_fail(ZOS_PCST_ZT_FAIL_GETINFO);
	}

	/* 5. Normal-console output through the syscall. */
	zt_write_str(bank_prefix, sizeof(bank_prefix) - 1);
	scratch_char = (unsigned char)('0' + info.bank);
	zt_write_str(&scratch_char, 1);
	zt_write_str(pid_sep, sizeof(pid_sep) - 1);
	scratch_char = (unsigned char)('0' + info.pid);
	zt_write_str(&scratch_char, 1);
	zt_write_str(line_end, sizeof(line_end) - 1);
	zt_write_str(ok_line, sizeof(ok_line) - 1);

	/* 6. Known exit status carrying the bank that executed this image. */
	exit(ZOS_PCST_ZEXTEST_PASS | info.bank);
	return 0; /* unreachable */
}
