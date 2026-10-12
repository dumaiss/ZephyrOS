/* ZephyrOS kernel interface for M0.
 *
 * Declares the M0 kernel entry, the fatal-error path, and the early serial
 * console.  These are internal implementation details of the ZephyrOS
 * bring-up; they are not a stable ZephyrOS ABI.
 */

#ifndef ZEPHYROS_KERNEL_H
#define ZEPHYROS_KERNEL_H

#include "platform.h"

/* Kernel entry.  Called once from the common boot entry (mode 11 active,
 * bank 7 visible, interrupts disabled).  Does not return. */
void kernel_main(void);

/* Fatal error path.  Prints "PANIC: <message>" through the early console
 * when possible, disables interrupts, and enters the controlled halt loop.
 * Does not return.  The message pointer may be null. */
void panic(const char *message);

/* Early serial console (SIO0/B, polled, 115200 8N1).  The whole path is
 * resident in the permanent common region so diagnostics survive
 * mode/bank transitions and kernel failures. */
void early_console_init(void);
void early_console_putc(char ch);
void early_console_puts(const char *s);

#endif /* ZEPHYROS_KERNEL_H */
