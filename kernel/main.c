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

#include "kernel/event/event.h"

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
 * The launcher starts the configured boot program - the ROMFS shell in
 * the production images - through its `/system` VFS path via the
 * unchanged `process_exec()` path.  `KOS_BOOT_PROGRAM` (the selected
 * system file, systems/<name>.json) selects the program, and
 * `after_exit` selects what happens after a normal exit: `relaunch`
 * starts the same program again, `monitor` ends the run at
 * `/system/monitor.exe`, which is what every acceptance image wants - one
 * run, then a prompt.  Changing either is one build setting plus the EXE
 * in the system's store, with no kernel edit.
 *
 * Every cycle is one line: `boot: <path> exit <n>` for a normal exit and
 * `boot: <path> failed <n>` when the program could not be run at all
 * (missing, refused or faulty).  A failed boot program falls back to the
 * monitor, whose own failure is the controlled halt; a monitor that exits
 * hands the machine back to the boot program.  Both lines go through the
 * console driver when the build has one and on the early console
 * otherwise, like the panic path. */

/* Configured boot program and after-exit policy: tools/systemgen.py
 * validates both against the store it wrote and the build writes them
 * into boot_program.h (the SDCC 4.5.0 driver cannot take the ".exe"
 * path through -D). */
static const char kernel_boot_program[] = KOS_BOOT_PROGRAM;

/* The one fallback program, fixed by the launcher policy: an image that
 * wants it in the store lists it (tools/systemgen.py checks). */
static const char kernel_boot_monitor[] = "/system/monitor.exe";

/* Powers of ten, high to low: the kernel links no arithmetic runtime, so
 * decimal output subtracts instead of dividing. */
static const uint16_t kernel_boot_dec_divisors[5] = {
	10000u, 1000u, 100u, 10u, 1u
};

static char *kernel_boot_put_dec(char *out, uint16_t value)
{
	uint8_t started = 0u;
	uint8_t index;

	for (index = 0u; index < 5u; ++index) {
		uint16_t divisor = kernel_boot_dec_divisors[index];
		uint8_t digit = 0u;

		while (value >= divisor) {
			value = (uint16_t)(value - divisor);
			++digit;
		}
		if (digit != 0u || started != 0u || divisor == 1u) {
			*out++ = (char)('0' + digit);
			started = 1u;
		}
	}
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

/* Reset the console to its start state before the boot program starts:
 * the terminal goes back to its defaults on a clean screen and no input
 * typed for the previous program is left for the next one.  A build
 * without a console driver has nothing to reset and says so. */
static int kernel_console_reset(void)
{
	struct kos_ipc_frame frame;
	uint8_t handle = kernel_console_handle();

	if (handle == 0u) {
		return -KOS_ENODEV;
	}
	frame.op = KCONSOLE_OP_RESET;
	frame.flags = 0u;
	frame.desc = KOS_IPC_DESC_NONE;
	frame.arg0 = 0u;
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

/* One launcher line: `boot: <path> exit <n>` or `boot: <path> failed
 * <n>`.  The path can be as long as the setting allows, so the line is
 * built in one buffer and emitted through the console path (every emit
 * falls back to the early console, which needs the terminator). */
static void kernel_boot_line(const char *program, const char *event,
			     uint16_t value)
{
	char line[KOS_PATH_MAX + 24];
	char *out = line;
	const char *scan;

	for (scan = "boot: "; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = program; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = event; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	out = kernel_boot_put_dec(out, value);
	*out++ = '\r';
	*out++ = '\n';
	*out = '\0';
	kernel_boot_emit(line, (uint16_t)(out - line));
}

/* The controlled halt: the monitor itself could not be started, so no
 * program can report anything again.  Never returns. */
static void kernel_boot_halt(void)
{
	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}

static void kernel_boot_launcher(void)
{
	/* Which program the next cycle runs is the whole policy state: the
	 * boot program, or the monitor the launcher itself fell back to. */
	const char *program = kernel_boot_program;

	for (;;) {
		uint16_t status = 0;
		int result;

		/* Every start begins on a console reset: a program that
		 * left the display reprogrammed, or input it never read,
		 * must not shape its successor's start.  A machine with no
		 * console driver has nothing to reset and boots as
		 * before. */
		(void)kernel_console_reset();
		result = process_exec(program, &status);
		if (result != EXE_EXEC_OK) {
			/* The program could not be run at all: name it and
			 * fall back to the monitor, where an owner can look
			 * at the store.  The monitor's own failure is the
			 * end of the line. */
			kernel_boot_line(program, " failed ",
					 (uint16_t)(0u - (uint16_t)result));
			if (program != kernel_boot_program) {
				kernel_boot_halt();
			}
			program = kernel_boot_monitor;
			continue;
		}
		kernel_boot_line(program, " exit ", status);
		if (program != kernel_boot_program) {
			/* The fallback ran: back to the boot program. */
			program = kernel_boot_program;
		}
#if KOS_BOOT_AFTER_MONITOR
		else {
			/* An acceptance image: one run, then the monitor. */
			program = kernel_boot_monitor;
		}
#endif
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
	/* The common region arrives as the reset copier wrote it: FFh for
	 * everything the ROM image does not carry, and the image carries no
	 * _COMMON_DATA bytes, so the event table and the per-process flags
	 * are invalid until they are cleared here.  The clear must happen
	 * before interrupts can be enabled - the delivery checks in the
	 * common region (kos_event_irq_exit, kos_event_syscall_exit) read
	 * the table on every interrupt and syscall return, and a warm reboot
	 * can leave the timer driver's channels requesting.  Nothing before
	 * irq_init() unmasks interrupts (irq_init() itself runs `di`), so
	 * this order is the warranty; the warm reboot stops the CTC channels
	 * before it jumps to the reset vector (syscall_reboot). */
	kos_event_init();
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
