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
#include "zephyros/module.h"
#include "zephyros/romfs.h"
#include "zephyros/process.h"
#include "zephyros/service.h"

#include "zos/protocol.h"

#include "kernel/ipc/ipc.h"
#include "kernel/irq/irq_private.h"
#include "kernel/memory/banking.h"
#include "kernel/vfs/vfs.h"

/* Single integration seam for target acceptance builds; the runner and
 * the acceptance-group selection live in tests/target/support/target_test.c. */
#ifndef ZOS_TARGET_TEST_BUILD
#define ZOS_TARGET_TEST_BUILD 0
#endif

#if ZOS_TARGET_TEST_BUILD
/* TEST-ONLY HOOK: target acceptance runner; implementation and milestone
 * selection live in tests/target/support/target_test.c (acceptance images
 * only).  The early hook runs before the boot module load; the late hook
 * runs after console binding. */
void zos_target_test_early(void);
void zos_target_test_late(void);
/* Boot-time acceptance modules: loaded after the production boot set and
 * before the normal mount pass.  Implementation lives in the test tree. */
void zos_boot_test_modules(void);
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

/* IOC-FAT root filesystem boot load (package 30).  Production carries
 * `iocfat.zmod` in the recovery store and loads it last, after the console
 * devices.  The module discovers the IOC class, registers the `iocfat`
 * provider and a failed init (missing IOC, a CAPS failure or an unknown
 * command) is rolled back by the loader and reported; the SIO fallback and
 * every earlier device remain usable.  The optional `/` mount is
 * instantiated by the normal mount pass.  Acceptance images that replace the
 * boot set compile this block out. */
#ifndef ZOS_IOCFAT_BOOT
#define ZOS_IOCFAT_BOOT 1
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

/* Failure-only early-SIO diagnostic for one optional boot module: the
 * module name plus the two-hex-digit ZOS_E* code behind the failed load
 * (the loader and ROMFS return negated codes).  A missing ROMFS entry
 * (0x05), a malformed container (0x0A), an allocation that no longer
 * fits (0x06) and a refused init (0x0B) therefore all distinguish on
 * the serial link without any code from the failed module being
 * executed.  The resident services are not optional - the manifest is
 * the image's boot contract - so a manifest that cannot be placed is
 * reported the same way and the boot continues to the mount pass, where
 * the failure shows up as the console and `/` being unusable.
 *
 * Compiled only when an optional load exists, so pre-M8 acceptance
 * images (which compile every optional block out) keep their bytes. */
#if ZOS_IOCFAT_BOOT
static void kernel_module_unavailable(const char *name, int result)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned code = (unsigned)(-result) & 0xFFu;

	early_console_puts(name);
	early_console_puts(": unavailable 0x");
	early_console_putc(hex[(code >> 4) & 0x0Fu]);
	early_console_putc(hex[code & 0x0Fu]);
	early_console_puts("\r\n");
}
#endif

static void kernel_service_manifest_failed(int result)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned code = (unsigned)(-result) & 0xFFu;

	early_console_puts("services: unavailable 0x");
	early_console_putc(hex[(code >> 4) & 0x0Fu]);
	early_console_putc(hex[code & 0x0Fu]);
	early_console_puts("\r\n");
}

/* Module-phase loader: open `/SYSTEM/<name>.zmod` through VFS and stream it
 * into the unchanged M4 loader through the bounded zmod_source adapter.  The
 * path lives in the shared bank-7 VFS workspace path slot and the file
 * handle in static storage, so the boot path is free of gate-stack-sized
 * automatic objects (work/M9-REFERENCE.md).  Non-static so the host replay
 * can drive the same VFS-backed load path. */
static struct vfs_file kernel_boot_file;

int kernel_load_vfs_module(const char *name)
{
	static const char prefix[] = "/SYSTEM/";
	static const char suffix[] = ".zmod";
	struct zmod_source source;
	/* The boot set is a handful of compile-time names, so the path is
	 * assembled in the shared bank-7 VFS workspace path slot instead of a
	 * dedicated buffer; no syscall is active during the module phase. */
	char *out = VFS_WS_IN_A;
	const char *scan;
	int result;

	/* The module phase runs before any process exists, so the path view
	 * is provably free; claiming it here keeps the guard's invariant
	 * ("the two views are never live together") true for boot too. */
	vfs_ws_enter(VFS_WS_VIEW_PATH);
	for (scan = prefix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = name; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	for (scan = suffix; *scan != '\0'; ++scan) {
		*out++ = *scan;
	}
	*out = '\0';

	result = vfs_open(VFS_WS_IN_A, ZFS_O_READ, &kernel_boot_file);
	if (result == 0) {
		result = vfs_file_source(&kernel_boot_file, &source);
		if (result == 0) {
			result = module_load_source(name, &source);
		}
		(void)vfs_close(&kernel_boot_file);
	}
	vfs_ws_leave(VFS_WS_VIEW_PATH);
	return result;
}

/* Resolve the boot path through the selected source.  A corrupt recovery
 * store fails here through the early-console panic path, before any
 * image code is ever executed.
 *
 * Production boot order is explicit and deterministic (no general
 * dependency resolver).  The nucleus mounts the recovery store, reads the
 * boot manifest and hands it to the resident service loader, which places
 * every service the manifest lists into bank 6 and runs their boot tiers
 * and inits in dependency order (architecture 36.4).  The store's manifest
 * is what the image boots: production lists the five driver services, and
 * an image that needs the serial-only fallback lists a subset.  The arena
 * modules the store still carries (the IOC-FAT provider) load through the
 * unchanged module loader after the services, and the mount pass runs last.
 */
static uint8_t kernel_boot_module(void)
{
	int result = vfs_init();

	if (result != 0) {
		romfs_diagnose(result);
		panic("romfs: corrupt recovery image");
	}
	result = service_load_all(ZOS_SERVICE_MANIFEST_NAME);
	if (result != 0) {
		kernel_service_manifest_failed(result);
	}
#if ZOS_IOCFAT_BOOT
	/* The IOC-FAT root filesystem provider, an arena module.  It
	 * discovers the IOC class through the nucleus's class shim (the
	 * service owns the transport) and registers the `iocfat` provider;
	 * a missing IOC endpoint, a CAPS failure or an unknown command
	 * fails its init and the loader rolls it back.  The optional `/`
	 * mount is instantiated by the mount pass below, so an absent or
	 * broken card leaves `/SYSTEM` and the console fully usable. */
	result = kernel_load_vfs_module("iocfat");
	if (result < 0) {
		kernel_module_unavailable("iocfat", result);
	}
#endif
#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: acceptance-only boot modules (tests/target). */
	zos_boot_test_modules();
#endif
	/* Normal mount phase: production has only the system entry, so this
	 * is a no-op; acceptance manifests exercise optional/required
	 * mounts here (architecture section 32.32.4). */
	(void)vfs_mount_all();
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

/* --- native boot launcher (M9 shell readiness, package 28b) ------------
 *
 * The launcher starts the configured boot program - the ROMFS
 * `monitor.zex` by default - through its `/SYSTEM` VFS path via the
 * unchanged `process_exec()` path, and re-execs it after every normal
 * exit.  `ZOS_BOOT_PROGRAM` (config/system.mk) selects the program: M10's
 * shell is one build setting plus its ZEX in the recovery manifest, with
 * no kernel edit.
 *
 * The child's exit status and the recovered process/bank counts are
 * printed after each cycle (normal console when bound, early console
 * otherwise); a launch failure names the configured path on the early
 * console and takes the controlled halt path. */

/* Configured boot program; the build validates it (tools/bootprogram.py). */
static const char kernel_boot_program[] = ZOS_BOOT_PROGRAM;

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

/* The console service endpoint, looked up once (0 while the store
 * provided none). */
static uint8_t kernel_console_endpoint;

static uint8_t kernel_console_handle(void)
{
	if (kernel_console_endpoint == 0u) {
		uint8_t version = 0u;
		int8_t handle = ipc_lookup(ZOS_CLASS_CONSOLE,
					   ZOS_CLASS_INSTANCE, &version);

		if (handle > 0 && version == ZOS_PROTOCOL_VERSION) {
			kernel_console_endpoint = (uint8_t)handle;
		}
	}
	return kernel_console_endpoint;
}

/* One console byte from the nucleus: the console protocol's PUTC form, the
 * one operation a caller without a descriptor can drive (36.7). */
static int kernel_console_putc(uint8_t handle, uint8_t ch)
{
	struct zos_ipc_frame frame;

	frame.op = ZCONSOLE_OP_PUTC;
	frame.flags = 0u;
	frame.desc = ZOS_IPC_DESC_NONE;
	frame.arg0 = ch;
	frame.arg1 = 0u;
	return ipc_nucleus_call(handle, &frame);
}

/* A boot line through the console service: 0 when every byte was accepted,
 * the negated error otherwise (the caller then stays on the early
 * console). */
static int kernel_console_write(const char *line, uint16_t length)
{
	uint8_t handle = kernel_console_handle();

	if (handle == 0u) {
		return -ZOS_ENODEV;
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

/* The boot path's own output: the console service composes the same
 * input/output pair the M8 console device did, and the early console is
 * what remains when the store provided no console service (the fallback
 * rule the panic path also depends on). */
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

		if (result != ZEX_EXEC_OK) {
			kernel_boot_fail((uint16_t)(0u - (uint16_t)result));
		}
		kernel_boot_report(status);
	}
}
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

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: subsystem acceptance phase (target_test.c). */
	zos_target_test_early();
#endif

	/* The early console above stays authoritative: it, and the panic
	 * path, work with no service at all.  The module loader is then
	 * initialized, the resident service set and the arena modules load
	 * from the recovery store, and the console service - if the store
	 * booted one - becomes what the boot path prints through.  Load
	 * diagnostics go to the early console before any service output. */
	{
		uint8_t module_failures;

		(void)module_loader_init();
		module_failures = kernel_boot_module();
		if (module_failures != 0) {
			early_console_puts("console: service failed\r\n");
		} else if (kernel_console_handle() == 0u) {
			early_console_puts("console: unbound\r\n");
		} else {
			static const char console_ok[] =
				"console: service OK\r\n";

			if (kernel_console_write(console_ok,
						 (uint16_t)(sizeof(console_ok) -
							    1u)) != 0) {
				early_console_puts("console: unavailable\r\n");
			}
		}
	}

#if ZOS_TARGET_TEST_BUILD
	/* TEST-ONLY HOOK: subsystem/M8/M9 acceptance phases (target_test.c). */
	zos_target_test_late();
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
