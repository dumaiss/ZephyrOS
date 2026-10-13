/* KonaOS kernel entry.
 *
 * Preconditions established by boot/start.asm:
 *   - interrupts disabled,
 *   - memory mode 11 (OS mode over bank 0), bank 7 visible at 2000h-DFFFh,
 *   - valid common stack,
 *   - SDCC C runtime state initialized,
 *   - early console initialized,
 *   - boot-time latch assertion passed.
 *
 * kernel_main() first re-validates the latch state that the bank-7 body
 * depends on, exercises the banking primitives and prints the banner
 * (validation itself is silent), then brings up the IRQ executive, the
 * VFS and the drivers and runs the boot launcher.  It does not return.
 */

#include "kernel.h"
#include "irq_types.h"
#include "romfs.h"
#include "process.h"
#include "mod.h"
#include "boot_program.h"

#include "kos/protocol.h"

#include "kernel/ipc/ipc.h"
#include "kernel/vfs/vfs.h"
#include "kernel/irq/irq_private.h"
#include "kernel/memory/banking.h"

/* Single integration seam for target acceptance builds; the runner and
 * the acceptance-group selection live in tests/target/support/target_test.c. */
#ifndef KOS_TARGET_TEST_BUILD
#define KOS_TARGET_TEST_BUILD 0
#endif

#if KOS_TARGET_TEST_BUILD
/* TEST-ONLY HOOK: target acceptance runner; implementation and group
 * selection live in tests/target/support/target_test.c (acceptance images
 * only).  The early hook runs before the boot program load; the late hook
 * runs after console binding. */
void kos_target_test_early(void);
void kos_target_test_late(void);
#endif

/* Hardware-smoke host sync.  Diagnostic images built with
 * KOS_SMOKE_HOST_SYNC=1 (the smoke-tested images) replace the blind
 * one-second connect delay with a wait for the host's two-byte sync
 * sequence on SIO0/B before the banner.  On this bench the board's reset
 * line also resets the USB bridge, so the boot log is only capturable if
 * the board holds off until the host has reopened the port and sent the
 * sequence (tools/serial_smoke.py sends it once the port is open and
 * repeats it until the first byte arrives).  Production and every other
 * image compile the gate out. */
#ifndef KOS_SMOKE_HOST_SYNC
#define KOS_SMOKE_HOST_SYNC 0
#endif

/* Failure-only hardware bring-up aid (never runs on a healthy image):
 * report the raw init status, the live latch value, and the first eight
 * bytes of recovery page 2 offset 0 staged by the ROM-access primitive.
 * A genuinely corrupt image shows the real "KRFS" magic here; a wrong
 * mapping or page selection shows different bytes instead of costing a
 * flash cycle.  The success transcript never sees these lines. */
static void romfs_diagnose(int result)
{
	static const char hex[] = "0123456789ABCDEF";
	struct rom_access_req probe;
	uint8_t status;
	uint8_t latch;
	uint8_t i;
	unsigned code = (unsigned)(-result);

	early_console_puts("romfs: init failed, code 0x");
	early_console_putc(hex[(code >> 4) & 0x0F]);
	early_console_putc(hex[code & 0x0F]);
	latch = platform_memctl_read();
	early_console_puts(", latch 0x");
	early_console_putc(hex[(latch >> 4) & 0x0F]);
	early_console_putc(hex[latch & 0x0F]);

	probe.page = KOS_ROMFS_FIRST_PAGE;
	probe.offset = 0;
	probe.length = 8;
	status = rom_access_copy(&probe);
	if (status == 0) {
		early_console_puts(", header");
		for (i = 0; i < 8; ++i) {
			uint8_t b = rom_access_staging[i];

			early_console_putc(' ');
			early_console_putc(hex[(b >> 4) & 0x0F]);
			early_console_putc(hex[b & 0x0F]);
		}
	} else {
		early_console_puts(", probe status 0x");
		early_console_putc(hex[(status >> 4) & 0x0F]);
		early_console_putc(hex[status & 0x0F]);
	}
	early_console_puts("\r\n");
}

static void kernel_driver_manifest_failed(int result)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned code = (unsigned)(-result) & 0xFFu;

	early_console_puts("drivers: unavailable 0x");
	early_console_putc(hex[(code >> 4) & 0x0Fu]);
	early_console_putc(hex[code & 0x0Fu]);
	early_console_puts("\r\n");
}

/* Resolve the boot path through the selected source.  A corrupt recovery
 * store fails here through the early-console panic path, before any image
 * code is ever executed.
 *
 * Production boot order is explicit and deterministic (no general
 * dependency resolver).  The nucleus initializes the namespace (the ROMFS
 * reader, the handle table and the in-process FILE endpoint), reads the
 * boot manifest and hands it to the resident driver loader, which places
 * every driver the manifest lists into bank 6 and runs their boot tiers
 * and inits in dependency order (architecture 4.1).  The store's manifest
 * is what the image boots: production lists the five driver drivers (the
 * selected filesystem driver among them, publishing `/fs`), and an image
 * that needs the serial-only fallback lists a subset.  The drivers are
 * the whole bank-6 population: there is no separate module pass.
 */
static uint8_t kernel_boot_drivers(void)
{
	int result = vfs_init();

	if (result != 0) {
		romfs_diagnose(result);
		panic("romfs: corrupt recovery image");
	}
	result = driver_load_all(KOS_DRIVER_MANIFEST_NAME);
	if (result != 0) {
		kernel_driver_manifest_failed(result);
		return 1u;
	}
	return 0;
}

/* ~1 second no-op delay at kernel entry (Z80 @ 10 MHz, no wait states) so
 * the operator can connect the serial terminal before the banner prints.
 *
 * Cycle math: the inner djnz loop with B=0 runs 256 passes
 * (255*13 + 8 = 3323 cycles); one outer pass is 7 + 3323 + 6 + 4 + 4 + 12
 * = 3356 cycles (3351 on the final pass).  2979 passes plus setup totals
 * 9,997,529 cycles ≈ 1.00 s.  Purely a no-op spin; no I/O is touched.
 *
 * Not static: the host replays (tests/test_m3_sim.py) recognize this symbol
 * to skip the wall-clock delay when replaying kernel_main. */
void kernel_serial_connect_delay(void)
{
	__asm__(
		"ld de,#0x0BA3\n"	/* 2979 outer passes */
		"100$:\n"
		"ld b,#0x00\n"		/* 256 inner passes */
		"101$:\n"
		"djnz 101$\n"
		"dec de\n"
		"ld a,d\n"
		"or e\n"
		"jr nz,100$\n"
	);
}

static void kernel_assert_os_mode(void)
{
	uint8_t latch = platform_memctl_read();

	if ((latch & (KOS_MODE_MASK | KOS_BANK_MASK)) != KOS_MODE_OS) {
		panic("mode 11 not active at kernel entry");
	}
}

/* Exercise the banking primitives.  Mode 11 keeps bank 7 visible at
 * 2000h-DFFFh regardless of the bank field, so bank updates are safe
 * here and must preserve the mode/page bits and reject bank 7. */
static void kernel_exercise_banking(void)
{
	uint8_t bank = platform_bank_get();
	uint8_t other = (bank == 0) ? 1 : 0;

	platform_bank_set(other);
	if (platform_bank_get() != other) {
		panic("bank update failed");
	}
	platform_bank_set(bank);
	if (platform_bank_get() != bank) {
		panic("bank restore failed");
	}
	platform_bank_set(7);
	if (platform_bank_get() != bank) {
		panic("bank 7 not rejected");
	}
}

/* --- native boot launcher -------------------------------------------
 *
 * The launcher starts the configured boot program - the ROMFS
 * `monitor.exe` by default - through its `/system` VFS path via the
 * unchanged `process_exec()` path, and re-execs it after every normal
 * exit.  `KOS_BOOT_PROGRAM` (the selected system file, systems/<name>.json)
 * selects the program: changing it (for example to a shell) is one build
 * setting plus the EXE in the system's store, with no kernel edit.
 *
 * The child's exit status and the recovered process/bank counts are
 * printed after each cycle (normal console when bound, early console
 * otherwise); a launch failure names the configured path on the early
 * console and takes the controlled halt path. */

/* Configured boot program: tools/systemgen.py validates it against the
 * store it wrote and the build writes it into boot_program.h (the SDCC
 * 4.5.0 driver cannot take the ".exe" path through -D). */
static const char kernel_boot_program[] = KOS_BOOT_PROGRAM;

static char *kernel_boot_put_hex16(char *out, uint16_t value)
{
	static const char digits[] = "0123456789ABCDEF";
	uint8_t shift;

	for (shift = 0; shift < 16; shift += 4) {
		*out++ = digits[(value >> (12 - shift)) & 0x0Fu];
	}
	return out;
}

static char *kernel_boot_put_dec(char *out, uint8_t value)
{
	uint8_t hundreds = 0;
	uint8_t tens = 0;

	while (value >= 100u) {
		value = (uint8_t)(value - 100u);
		++hundreds;
	}
	while (value >= 10u) {
		value = (uint8_t)(value - 10u);
		++tens;
	}
	if (hundreds != 0) {
		*out++ = (char)('0' + hundreds);
	}
	if (hundreds != 0 || tens != 0) {
		*out++ = (char)('0' + tens);
	}
	*out++ = (char)('0' + value);
	return out;
}

/* The console driver endpoint, resolved once from the host-built
 * publication the resident loader installed (0 while the store provided
 * none).  There is no runtime class lookup (architecture 5.3). */
static uint8_t kernel_console_endpoint;

static uint8_t kernel_console_handle(void)
{
	if (kernel_console_endpoint == 0u) {
		kernel_console_endpoint = vfs_ns_publication_endpoint(
			"/console");
	}
	return kernel_console_endpoint;
}

/* One console byte from the nucleus: the console protocol's PUTC form, the
 * one operation a caller without a descriptor can drive (4.9). */
static int kernel_console_putc(uint8_t handle, uint8_t ch)
{
	struct kos_ipc_frame frame;

	frame.op = KCONSOLE_OP_PUTC;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = ch;
	frame.arg1 = 0u;
	return ipc_nucleus_call(handle, &frame);
}

/* A boot line through the console driver: 0 when every byte was accepted,
 * the negated error otherwise (the caller then stays on the early
 * console). */
static int kernel_console_write(const char *line, uint16_t length)
{
	uint8_t handle = kernel_console_handle();

	if (handle == 0u) {
		return -KOS_ENODEV;
	}
	while (length != 0u) {
		int result = kernel_console_putc(handle, (uint8_t)*line);

		if (result < 0) {
			return result;
		}
		++line;
		--length;
	}
	return 0;
}

/* The boot path's own output: through the console driver when the build
 * has one, and on the early console otherwise (the same path the panic
 * handler depends on). */
static void kernel_boot_emit(const char *line, uint16_t length)
{
	uint8_t handle = kernel_console_handle();
	uint16_t index;

	if (handle != 0u) {
		for (index = 0u; index < length; ++index) {
			if (kernel_console_putc(handle,
						(uint8_t)line[index]) < 0) {
				break;
			}
		}
		if (index == length) {
			return;
		}
	}
	early_console_puts(line); /* line is always NUL-terminated */
}

/* Report one normal exit with the recovered counts.  A repeated identical
 * line proves the exec/exit cycle leaked no process state. */
static void kernel_boot_report(uint16_t status)
{
	char line[64];
	static const char prefix[] = "monitor: exit status 0x";
	static const char active[] = " active ";
	static const char free_banks[] = " free-banks ";
	char *out = line;
	const char *scan;

	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_boot_put_hex16(out, status);
	for (scan = active; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_boot_put_dec(out, process_active_count());
	for (scan = free_banks; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_boot_put_dec(out, process_free_banks());
	*out++ = '\r';
	*out++ = '\n';
	*out = '\0';
	kernel_boot_emit(line, (uint16_t)(out - line));
}

/* Launch failure: name the configured program on the early console and
 * take the controlled halt loop.  Never returns.  The path is printed in
 * pieces so the diagnostic needs no buffer as long as the setting. */
static void kernel_boot_fail(uint16_t code)
{
	char text[5];
	char *out = kernel_boot_put_hex16(text, code);

	*out = '\0';
	early_console_puts(kernel_boot_program);
	early_console_puts(": launch failed 0x");
	early_console_puts(text);
	early_console_puts("\r\n");
	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}

static void kernel_boot_launcher(void)
{
	for (;;) {
		uint16_t status = 0;
		int result = process_exec(kernel_boot_program, &status);

		if (result != EXE_EXEC_OK) {
			kernel_boot_fail((uint16_t)(0u - (uint16_t)result));
		}
		kernel_boot_report(status);
	}
}
#if KOS_SMOKE_HOST_SYNC
/* TEST-ONLY HOOK: hold the first banner byte until the host is listening.
 * The SIO0/B receiver is live from early_console_init (boot/start.asm);
 * the channel reset that follows a board reset clears any stale bytes, so
 * the wait is only entered on a fresh boot.  The two-state scan discards
 * every byte that does not continue the sequence, which makes repeated
 * sends and glitch bytes harmless. */
static __sfr __at KOS_SIO0B_CTRL kernel_host_sync_ctrl;
static __sfr __at KOS_SIO0B_DATA kernel_host_sync_data;

static void kernel_host_sync_wait(void)
{
	uint8_t matched_first = 0;

	for (;;) {
		uint8_t ch;

		while ((kernel_host_sync_ctrl & KOS_SIO_RR0_RX_AVAILABLE)
		       == 0u) {
			/* Nothing has been printed yet. */
		}
		ch = kernel_host_sync_data;
		if (ch == KOS_SMOKE_HOST_SYNC_FIRST) {
			matched_first = 1u;
		} else if (matched_first != 0u &&
			   ch == KOS_SMOKE_HOST_SYNC_SECOND) {
			return;
		} else {
			matched_first = 0u;
		}
	}
}
#endif /* KOS_SMOKE_HOST_SYNC */

void kernel_main(void)
{
#if KOS_SMOKE_HOST_SYNC
	/* TEST-ONLY HOOK: hold the first byte until the host is listening
	 * (bench quirk: the board's reset also resets the USB bridge). */
	kernel_host_sync_wait();
#else
	kernel_serial_connect_delay();
#endif
	kernel_assert_os_mode();
	kernel_exercise_banking();
	irq_init();

	early_console_puts("KonaOS 0.0\r\n");
	early_console_puts("platform: Zephyr-80\r\n");
	early_console_puts("kernel entry: OK\r\n");
	early_console_puts("serial: OK\r\n");

#if KOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: subsystem acceptance phase (target_test.c). */
	kos_target_test_early();
#endif

	/* The early console above stays authoritative: it, and the panic
	 * path, work with no driver at all.  The resident driver set loads
	 * from the recovery store, and the console driver - if the store
	 * booted one - becomes what the boot path prints through.  Load
	 * diagnostics go to the early console before any driver output. */
	{
		uint8_t driver_failures = kernel_boot_drivers();

		if (driver_failures != 0) {
			/* A required driver failed to initialize: the loader
			 * named it on the early console.  The boot fails
			 * here, before the launcher (5.3). */
			early_console_puts("console: driver failed\r\n");
			__asm__("di");
			for (;;) {
				__asm__("halt");
			}
		} else if (kernel_console_handle() == 0u) {
			early_console_puts("console: unbound\r\n");
		} else {
			static const char console_ok[] =
				"console: driver OK\r\n";

			if (kernel_console_write(console_ok,
						 (uint16_t)(sizeof(console_ok) -
							    1u)) != 0) {
				early_console_puts("console: unavailable\r\n");
			}
		}
	}

#if KOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: the acceptance images' late phases (target_test.c). */
	kos_target_test_late();
#endif

	/* Native boot launcher: exec the configured program through the
	 * process path; normal exits are reported and the program is
	 * re-executed (see kernel_boot_launcher above). */
	kernel_boot_launcher();

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
