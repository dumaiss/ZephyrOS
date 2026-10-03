/* ZephyrOS M3 console service (internal kernel interface).
 *
 * The console service is a thin binding layer over registered devices:
 * it stores separate input and output handles and never compares device
 * or driver names.  Default binding finds the first device advertising
 * ZCAP_CONSOLE_OUT / ZCAP_CONSOLE_IN.
 *
 * The early console (`early_console_*`) remains the permanent fallback
 * diagnostic path and is not routed through this service; kernel panic
 * continues to bypass it.
 *
 * Operations fail cleanly: ZDEV_ENODEV when nothing is bound,
 * ZDEV_ENOTSUP when the bound device does not implement the operation,
 * otherwise the driver operation result is passed through.
 */

#ifndef ZEPHYROS_CONSOLE_H
#define ZEPHYROS_CONSOLE_H

#include "zephyros/device.h"

/* Explicit binding: each handle may be ZDEV_HANDLE_INVALID to leave that
 * direction unbound.  Non-zero handles must reference registered devices
 * carrying the matching output/input console capability. */
int console_bind(zdev_handle_t input, zdev_handle_t output);

/* Default binding by capability; ZDEV_ENODEV if either direction has no
 * suitable device (binding is then left cleared). */
int console_bind_defaults(void);
void console_unbind(void);

zdev_handle_t console_input_handle(void);
zdev_handle_t console_output_handle(void);

/* Operations on the bound handles. */
int console_putc(char ch);
int console_write(const char *buf, uint16_t len);
int console_poll(void);
int console_getc(void);

#endif /* ZEPHYROS_CONSOLE_H */
