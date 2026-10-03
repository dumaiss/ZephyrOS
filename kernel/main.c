/* ZephyrOS M1 kernel entry.
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
 * depends on, exercises the banking primitives, prints the M0 banner, and
 * enters the controlled halt loop.  Successful validation is silent: the
 * banner remains byte-identical to M0.  It does not return.
 */

#include "zephyros/kernel.h"
#include "zephyros/irq.h"
#include "zephyros/console.h"
#include "zephyros/module.h"
#include "zephyros/romfs.h"
#include "zephyros/process.h"

/* Single integration seam for target acceptance builds; the runner and
 * milestone selection live in tests/target/support/target_test.c.  The
 * one unavoidable boot-phase hook (ZOS_M4_SIO_IRQ) is marked TEST-ONLY
 * HOOK below with its test-owned implementation path. */
#ifndef ZOS_TARGET_TEST_BUILD
#define ZOS_TARGET_TEST_BUILD 0
#endif

#if ZOS_TARGET_TEST_BUILD
/* TEST-ONLY HOOK: target acceptance runner; implementation and milestone
 * selection live in tests/target/support/target_test.c (acceptance images
 * only).  The early hook runs before the module catalog load; the late
 * hook runs after console binding. */
void zos_target_test_early(void);
void zos_target_test_late(void);
#endif

/* Set only by the m4-selftest acceptance build: keep CTC0 pending across
 * the boot-module load while tests/modules/sio_irq_module.c claims it. */
#ifndef ZOS_M4_SIO_IRQ
#define ZOS_M4_SIO_IRQ 0
#endif

/* Boot-module source selection.  Production and M5 images boot the SIO
 * ZMOD from the ROMFS recovery store (the default); the pre-M5
 * acceptance images set ZOS_ROMFS_BOOT=0 in the Makefile and keep their
 * embedded test catalogs. */
#ifndef ZOS_ROMFS_BOOT
#define ZOS_ROMFS_BOOT 1
#endif

/* IOC transport boot load.  Production and the M8 images carry
 * `ioc.zmod` in the recovery store and load it deterministically after
 * the SIO fallback; a failed load or a failed link bring-up is rolled
 * back by the loader and must not invalidate the already loaded SIO
 * console.  Pre-M8 acceptance images do not carry the module and compile
 * this block out so their accepted boot stays exactly as validated. */
#ifndef ZOS_IOC_BOOT
#define ZOS_IOC_BOOT 1
#endif

/* HID keyboard/text boot load.  Production and the M8 images carry
 * `hid.zmod` in the recovery store and load it after the IOC transport;
 * the module discovers IOC by class/capability and a missing or
 * malformed dependency is a load failure rolled back by the loader.
 * The SIO fallback and the IOC transport are never disturbed.  Pre-M8
 * acceptance images compile this block out so their accepted boot stays
 * exactly as validated. */
#ifndef ZOS_HID_BOOT
#define ZOS_HID_BOOT 1
#endif

/* V9958 video boot load.  Production and the M8 images carry
 * `v9958.zmod` (and its ROMFS DATA font) in the recovery store and load
 * it after HID; the module has no IOC dependency.  A missing font
 * resource or a failed VDP bring-up fails the module init, the loader
 * rolls the (empty) registration set back, and the SIO fallback, the
 * IOC transport and HID remain usable.  Pre-M8 acceptance images
 * compile this block out so their accepted boot stays exactly as
 * validated. */
#ifndef ZOS_V9958_BOOT
#define ZOS_V9958_BOOT 1
#endif

/* Temporary pre-shell launcher selection.  Production and the M7
 * acceptance image (ZOS_MONITOR_BOOT=1, the default) run `monitor.zex`
 * through the unchanged M6 exec path and re-exec it after every normal
 * exit.  Earlier acceptance images set ZOS_MONITOR_BOOT=0 so their
 * transcripts and behavior stay exactly as accepted. */
#ifndef ZOS_MONITOR_BOOT
#define ZOS_MONITOR_BOOT 1
#endif

/* Hardware-smoke host sync.  Diagnostic images built with
 * ZOS_SMOKE_HOST_SYNC=1 (the M8 IOC smoke image) replace the blind
 * one-second connect delay with a wait for the host's two-byte sync
 * sequence on SIO0/B before the banner.  On this bench the board's reset
 * line also resets the USB bridge, so the boot log is only capturable if
 * the board holds off until the host has reopened the port and sent the
 * sequence (tools/serial_smoke.py sends it once the port is open and
 * repeats it until the first byte arrives).  Production and every other
 * image compile the gate out. */
#ifndef ZOS_SMOKE_HOST_SYNC
#define ZOS_SMOKE_HOST_SYNC 0
#endif

#if ZOS_ROMFS_BOOT
/* Failure-only hardware bring-up aid (never runs on a healthy image):
 * report the raw init status, the live latch value, and the first eight
 * bytes of recovery page 2 offset 0 staged by the ROM-access primitive.
 * A genuinely corrupt image shows the real "ZRFS" magic here; a wrong
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

	probe.page = ZOS_ROMFS_FIRST_PAGE;
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
#endif

/* Resolve and load the boot module through the selected source.  Returns
 * 0 when the module is resident, non-zero otherwise.  A corrupt recovery
 * store fails here through the early-console panic path, before any
 * module code is ever executed. */
static uint8_t kernel_boot_module(void)
{
#if ZOS_ROMFS_BOOT
	int result = romfs_init();

	if (result != 0) {
		romfs_diagnose(result);
		panic("romfs: corrupt recovery image");
	}
	if (romfs_load_zmod("sio", "sio.zmod") < 0) {
		return 1;
	}
#if ZOS_IOC_BOOT
	/* Deterministic boot order after the SIO fallback: the IOC
	 * command+bulk transport.  A module whose init fails (including a
	 * failed link bring-up) is rolled back by the loader and reported
	 * here; the console still binds to SIO below. */
	if (romfs_load_zmod("ioc", "ioc.zmod") < 0) {
		early_console_puts("ioc: unavailable\r\n");
	}
#endif
#if ZOS_HID_BOOT
	/* Then the HID keyboard/text input module.  HID depends on the
	 * IOC class; a missing/malformed dependency fails its init, the
	 * loader rolls the (empty) registration set back, and the SIO
	 * fallback plus the IOC transport remain usable. */
	if (romfs_load_zmod("hid", "hid.zmod") < 0) {
		early_console_puts("hid: unavailable\r\n");
	}
#endif
#if ZOS_V9958_BOOT
	/* Then the V9958 video core.  It has no IOC dependency and
	 * registers the VIDEO class device only after its full bring-up
	 * and the ROMFS font upload succeeded; a missing/undersized font
	 * or a failed VDP sequence fails the init and the loader rolls it
	 * back without disturbing the earlier devices.  The normal console
	 * stays on the SIO output fallback until the console package adds
	 * the V9958 character interface. */
	if (romfs_load_zmod("v9958", "v9958.zmod") < 0) {
		early_console_puts("v9958: unavailable\r\n");
	}
#endif
	return 0;
#else
	return module_catalog_load_all();
#endif
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

	if ((latch & (ZOS_MODE_MASK | ZOS_BANK_MASK)) != ZOS_MODE_OS) {
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

#if ZOS_MONITOR_BOOT
/* --- temporary pre-shell launcher (M7) --------------------------------
 *
 * Production and the M7 acceptance image run the ROMFS `monitor.zex`
 * through the unchanged M6 `process_exec()` path and re-exec it after
 * every normal exit, until the M10 shell replaces this policy.  The
 * child's exit status and the recovered process/bank counts are printed
 * after each cycle (normal console when bound, early console otherwise);
 * a launch failure is reported on the early console and takes the
 * controlled halt path. */

static char *kernel_monitor_put_hex16(char *out, uint16_t value)
{
	static const char digits[] = "0123456789ABCDEF";
	uint8_t shift;

	for (shift = 0; shift < 16; shift += 4) {
		*out++ = digits[(value >> (12 - shift)) & 0x0Fu];
	}
	return out;
}

static char *kernel_monitor_put_dec(char *out, uint8_t value)
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

static void kernel_monitor_emit(const char *line, uint16_t length)
{
	if (console_output_handle() != 0) {
		(void)console_write(line, length);
		return;
	}
	early_console_puts(line); /* line is always NUL-terminated */
}

/* Report one normal monitor exit with the recovered counts.  A repeated
 * identical line proves the exec/exit cycle leaked no process state. */
static void kernel_monitor_report(uint16_t status)
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
	out = kernel_monitor_put_hex16(out, status);
	for (scan = active; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_monitor_put_dec(out, process_active_count());
	for (scan = free_banks; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_monitor_put_dec(out, process_free_banks());
	*out++ = '\r';
	*out++ = '\n';
	*out = '\0';
	kernel_monitor_emit(line, (uint16_t)(out - line));
}

/* Launch failure: early console and the controlled halt loop.  Never
 * returns. */
static void kernel_monitor_fail(uint16_t code)
{
	char line[40];
	static const char prefix[] = "monitor: launch failed 0x";
	char *out = line;
	const char *scan;

	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_monitor_put_hex16(out, code);
	*out++ = '\r';
	*out++ = '\n';
	*out = '\0';
	early_console_puts(line);
	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}

static void kernel_monitor_launcher(void)
{
	for (;;) {
		uint16_t status = 0;
		int result = process_exec("monitor.zex", &status);

		if (result != ZEX_EXEC_OK) {
			kernel_monitor_fail((uint16_t)(0u - (uint16_t)result));
		}
		kernel_monitor_report(status);
	}
}
#endif /* ZOS_MONITOR_BOOT */

#if ZOS_SMOKE_HOST_SYNC
/* TEST-ONLY HOOK: hold the first banner byte until the host is listening.
 * The SIO0/B receiver is live from early_console_init (boot/start.asm);
 * the channel reset that follows a board reset clears any stale bytes, so
 * the wait is only entered on a fresh boot.  The two-state scan discards
 * every byte that does not continue the sequence, which makes repeated
 * sends and glitch bytes harmless. */
static __sfr __at ZOS_SIO0B_CTRL kernel_host_sync_ctrl;
static __sfr __at ZOS_SIO0B_DATA kernel_host_sync_data;

static void kernel_host_sync_wait(void)
{
	uint8_t matched_first = 0;

	for (;;) {
		uint8_t ch;

		while ((kernel_host_sync_ctrl & ZOS_SIO_RR0_RX_AVAILABLE)
		       == 0u) {
			/* Nothing has been printed yet. */
		}
		ch = kernel_host_sync_data;
		if (ch == ZOS_SMOKE_HOST_SYNC_FIRST) {
			matched_first = 1u;
		} else if (matched_first != 0u &&
			   ch == ZOS_SMOKE_HOST_SYNC_SECOND) {
			return;
		} else {
			matched_first = 0u;
		}
	}
}
#endif /* ZOS_SMOKE_HOST_SYNC */

void kernel_main(void)
{
#if ZOS_SMOKE_HOST_SYNC
	/* TEST-ONLY HOOK: hold the first byte until the host is listening
	 * (bench quirk: the board's reset also resets the USB bridge). */
	kernel_host_sync_wait();
#else
	kernel_serial_connect_delay();
#endif
	kernel_assert_os_mode();
	kernel_exercise_banking();
	irq_init();

	early_console_puts("ZephyrOS 0.0\r\n");
	early_console_puts("platform: Zephyr-80\r\n");
	early_console_puts("kernel entry: OK\r\n");
	early_console_puts("serial: OK\r\n");
	early_console_puts("HALT\r\n");

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: m1/m2 acceptance phases (target_test.c). */
	zos_target_test_early();
#endif

/* --- registry, module catalog, console service ---------------------
	 * The early console above stays authoritative.  The device registry
	 * is initialized first, the boot module is loaded through the M4
	 * loader (from ROMFS in production/M5, from the embedded test catalog
	 * in pre-M5 acceptance images), and only then is the console
	 * service bound.  Load and binding diagnostics go to the early
	 * console before any dynamic device is used. */
	device_registry_init();
	{
		uint8_t module_failures;

		(void)module_loader_init();
#if ZOS_M4_SIO_IRQ
		/* TEST-ONLY HOOK: keep CTC0 pending across the load until the
		 * acceptance module owns the source; the concrete test lives in
		 * tests/target/support/target_test.c and
		 * tests/modules/sio_irq_module.c. */
		{
			uint8_t irq_enabled = irq_arch_lock();

			platform_ctc0_test_start();
			module_failures = kernel_boot_module();
			platform_ctc0_stop();
			irq_arch_unlock(irq_enabled);
		}
#else
		module_failures = kernel_boot_module();
#endif
		if (module_failures != 0) {
			early_console_puts("console: dynamic driver failed\r\n");
		} else if (console_bind_defaults() != ZDEV_OK) {
			early_console_puts("console: unbound\r\n");
		} else {
			static const char m4_console_ok[] =
				"console: dynamic sio0.b OK\r\n";

			if (console_write(m4_console_ok,
					  (uint16_t)(sizeof(m4_console_ok) - 1u)) !=
				    (int)(sizeof(m4_console_ok) - 1u)) {
				early_console_puts("console: service failed\r\n");
			}
		}
	}

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: m3/m4 acceptance phases (target_test.c). */
	zos_target_test_late();
#endif

#if ZOS_MONITOR_BOOT
	/* Temporary pre-shell launcher: exec the ROMFS monitor through the
	 * unchanged M6 process path; normal exits are reported and the
	 * monitor is re-executed (see kernel_monitor_launcher above). */
	kernel_monitor_launcher();
#endif

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
