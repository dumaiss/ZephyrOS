/* ZephyrOS M9 SYS_EXEC child program (test artifact, packages 28/28b).
 *
 * Packed as fschild.zex and run by fstest.zex through zos_exec().  It
 * proves, from the child's side:
 *   - it received startup block v1 with the caller's tail byte-exact, and
 *     the user-space splitter decodes it (the block, tail length, argc and
 *     every argv word are echoed for the simulator/hardware transcript);
 *   - it inherited the parent's CWD (printed for the host/simulator);
 *   - no descriptor is inherited (fd 0 is owned by no one);
 *   - its own chdir does not disturb the parent;
 *   - it can open and write a file in the new CWD, then exit with known
 *     status 0x5A5A while leaving the descriptor open, so the parent can
 *     prove the kernel released it;
 *   - SYS_GETINFO reports its true (user) parent, its own slot and its
 *     own application bank.
 *
 * Any unexpected state exits with 0x100 | step so the failure is visible in
 * the parent's status round trip.
 */

#include "zephyros/fs.h"

#include "zex_runtime.h"
#include "zex_fs_runtime.h"
#include "zex_args.h"

#define CH_EINVAL 0x02
#define CH_ARGC_MAX 8

static char ch_cwd[130];
static char *ch_argv[CH_ARGC_MAX];
/* Working copy of the page-zero tail: the startup block lives below the
 * syscall buffer window (0100h), so anything handed to a syscall or a
 * runtime wrapper must be copied into program data first. */
static char ch_tail[ZEX_STARTUP_TAIL_MAX + 1];

static void ch_puts(const char *s)
{
	unsigned int n = 0;

	while (s[n] != '\0') {
		++n;
	}
	(void)zex_write(s, n);
}

static void ch_dec(unsigned int value)
{
	char b[3];
	unsigned int tens = 0;
	unsigned int hundreds = 0;

	while (value >= 100u) {
		value -= 100u;
		++hundreds;
	}
	while (value >= 10u) {
		value -= 10u;
		++tens;
	}
	if (hundreds != 0u) {
		b[0] = (char)('0' + hundreds);
		b[1] = (char)('0' + tens);
		b[2] = (char)('0' + value);
		(void)zex_write(b, 3);
	} else if (tens != 0u) {
		b[0] = (char)('0' + tens);
		b[1] = (char)('0' + value);
		(void)zex_write(b, 2);
	} else {
		b[0] = (char)('0' + value);
		(void)zex_write(b, 1);
	}
}

static void ch_fail(unsigned char step)
{
	ch_puts("fschild: FAIL\r\n");
	zex_exit((unsigned int)(0x0100u | step));
}

/* Echo the decoded startup block and argv for the transcript. */
static void ch_echo_args(void)
{
	struct zex_startup *sb = zos_args();
	int argc;
	int i;

	if (sb == (struct zex_startup *)0 ||
	    sb->version != ZEX_STARTUP_VERSION ||
	    sb->length > (unsigned char)ZEX_STARTUP_TAIL_MAX ||
	    sb->tail[sb->length] != '\0') {
		ch_fail(0x10u);
	}
	/* Copy out of page zero before parsing or printing: the startup
	 * block is readable, but it lies below the 0100h syscall buffer
	 * window, so a syscall argument must live in program data. */
	for (i = 0; i <= (int)sb->length; ++i) {
		ch_tail[i] = sb->tail[i];
	}
	argc = zos_split(ch_tail, ch_argv, CH_ARGC_MAX);

	ch_puts("fschild: startup v");
	ch_dec((unsigned int)sb->version);
	ch_puts(" tail ");
	ch_dec((unsigned int)sb->length);
	ch_puts("\r\n");
	ch_puts("fschild: argv ");
	ch_dec((unsigned int)argc);
	ch_puts("\r\n");
	for (i = 0; i < argc && i < CH_ARGC_MAX; ++i) {
		ch_puts("fschild: arg ");
		ch_dec((unsigned int)i);
		ch_puts("=");
		ch_puts(ch_argv[i]);
		ch_puts("\r\n");
	}
}

void ch_main(void)
{
	int r;
	int fd;

	ch_echo_args();

	r = zos_getcwd(ch_cwd, (unsigned int)sizeof(ch_cwd));
	if (r < 0) {
		ch_fail(0x01u);
	}
	ch_puts("fschild: cwd ");
	(void)zex_write(ch_cwd, (unsigned int)r);
	ch_puts("\r\n");

	/* Descriptors are not inherited: fd 0 belongs to no one. */
	if (zos_close(0u) != -(int)CH_EINVAL) {
		ch_fail(0x02u);
	}

	/* The child's chdir must not disturb the parent. */
	if (zos_chdir("/R2") != 0) {
		ch_fail(0x03u);
	}

	fd = zos_open("CHILD.TXT",
		      (unsigned char)(ZFS_O_WRITE | ZFS_O_CREATE |
				      ZFS_O_TRUNC));
	if (fd < 0) {
		ch_fail(0x04u);
	}
	if (zos_write((unsigned int)fd, "child", 5u) != 5) {
		ch_fail(0x05u);
	}

	/* SYS_GETINFO reports the true parent: this child was started by an
	 * ordinary process, so the kernel-parent flag is clear. */
	{
		struct zex_info info;

		if (zex_getinfo(&info) < 0 ||
		    info.flags != 0 ||
		    info.pid > 6u || info.bank > 6u ||
		    info.bank == 0u) {
			ch_fail(0x06u);
		}
	}
	/* Deliberately no close: the kernel must release the descriptor and
	 * the provider handle at exit. */
	zex_exit(0x5A5Au);
}
