/* KonaOS native monitor (user/monitor).
 *
 * Ordinary EXE v2 program linked into the user bank by the unchanged
 * loader.  All ordinary input/output goes through the SDK's console
 * functions (the CONSOLE protocol on the standard handles); every
 * memory, port, and system operation goes through the diagnostic
 * wrappers.  The monitor has no SIO/V9958/HID knowledge, no CP/M
 * dependency, and no private kernel symbols, so the same binary works
 * with either console driver.
 *
 * Command lines arrive on the console and, when the store publishes a
 * SERIAL object at `/dev/serial/0`, on that lane as well: the monitor binds the name through the ordinary typed protocol
 * and polls it beside the console, so production - whose console input
 * is the HID build - can still be driven by a terminal or a host tool on
 * the lane, and a build without that object is console-only.
 * Each byte is echoed to the source it came from.
 *
 * Command surface (case-insensitive hex, 80-character lines):
 *
 *   H / ?             help
 *   R                 system/bank/process information
 *   D addr len        dump the monitor's own bank
 *   DB bank addr len  dump application bank 0-6
 *   M addr value      modify one byte in the monitor's bank
 *   MB bank addr value modify one byte in application bank 0-6
 *   I port            read I/O port 10h-FFh
 *   O port value      write I/O port 10h-FFh
 *   X path [tail]     run a EXE from /system, print its status
 *   Q                 exit (resumes the kernel launcher)
 *
 * Intel HEX load/export, GO, APP, and CP/M register-snapshot semantics
 * are deliberately not ported.
 */

#ifndef KONAOS_USER_MONITOR_H
#define KONAOS_USER_MONITOR_H

#include "kos.h"

/* Command-line capacity: at most 80 characters plus the terminating NUL. */
#define MON_LINE_MAX (KOS_LINE_MAX + 1)

/* Monitor exit statuses; Q uses 0 and the launcher reports the rest. */
#define MON_EXIT_QUIT    0x0000
#define MON_EXIT_DIAG    0x0201 /* BANK_INFO/GETINFO unavailable         */
#define MON_EXIT_CONSOLE 0x0202 /* console driver failed                */
#define MON_EXIT_INTERNAL 0x0203 /* mon_main returned unexpectedly       */
#define MON_EXIT_LAUNCH  0x0204 /* launch contract violation (HL != 0)   */

/* Monitor identity resolved at startup. */
extern unsigned char mon_bank; /* own application bank 0-6            */
extern unsigned char mon_mask; /* bank-validity mask from BANK_INFO   */
extern unsigned char mon_line[MON_LINE_MAX];

/* Handlers (each receives the cursor just after the command letter and
 * owns all further parsing; trailing text is rejected as ERR: ARG). */
void mon_cmd_info(char *cursor);
void mon_cmd_help(char *cursor);
void mon_cmd_mem(char *cursor, unsigned char with_bank,
		 unsigned char modify);
void mon_cmd_io_in(char *cursor);
void mon_cmd_io_out(char *cursor);
void mon_cmd_exec(char *cursor);

/* Parse a bank byte and validate it against the kernel-reported mask. */
unsigned char mon_scan_bank(char **cursor, unsigned char *bank);

/* Print two/four uppercase hex digits through the console. */
void mon_put_hex8(unsigned int value);
void mon_put_hex16(unsigned int value);

#endif /* KONAOS_USER_MONITOR_H */
