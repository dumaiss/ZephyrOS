/* KonaOS fatal-error path.
 *
 * Prints the clearest available diagnostic through the early console,
 * disables interrupts, and enters the controlled halt loop.
 *
 * Compiled into the _COMMON code segment: panic and its string literals are
 * resident in the permanent common region, so the path can execute without
 * depending on any banked memory.
 */

#include "kernel.h"

void panic(const char *message)
{
	early_console_puts("PANIC: ");
	if (message) {
		early_console_puts(message);
	}
	early_console_puts("\r\n");

	__asm__("di");
	for (;;) {
		__asm__("halt");
	}
}
