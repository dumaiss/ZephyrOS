/* KonaOS resident-driver acceptance client.
 *
 * The driver acceptance images run their diagnostic inside the
 * nucleus (kernel/main.c calls kos_target_test_early/late), which makes
 * that diagnostic a *bank-7 client* of the resident drivers: it holds no
 * descriptor and cannot use `move`, so it drives each protocol through
 * the descriptor-less forms plus the staged buffers that drivers name in
 * their replies (architecture 4.9, docs/devices.md section 13).
 *
 * This helper is the one place that knows those gymnastics:
 *
 *   driver_cli_endpoint()   endpoint resolution through the host-built
 *                            `/dev` publication the class names, 0
 *                            when the image serves no such provider;
 *   driver_cli_call()       one ipc_nucleus_call with a caller-owned
 *                            frame, returning the frame status;
 *   driver_cli_stage_read()/
 *   driver_cli_stage_write() one bounded move through the common copy
 *                            gate (kernel/memory/user_copy.asm) against
 *                            the driver bank, chunked through the gate's
 *                            64-byte staging block;
 *   driver_cli_*()          protocol convenience forms (putc/getc/poll)
 *                            and the transcript writers the diagnostics
 *                            print on the early console.
 *
 * Transcript convention: every phase prints one `<tag>: <detail>` line and
 * the diagnostic's last line is `<name>: OK` or `<name>: FAIL <phase>`,
 * which is what the simulator replays pin.
 */

#ifndef KOS_TARGET_DRIVER_CLIENT_H
#define KOS_TARGET_DRIVER_CLIENT_H

#include "driver.h"

/* The resident driver bank: bank 7 is the nucleus, so bank 6 is the last
 * application bank (architecture 2.2).  The copy gate takes it directly. */
#define DRIVER_CLI_BANK 6u

/* Bytes per gate transfer (the gate's common staging block is 64 bytes,
 * kernel/memory/user_copy.asm). */
#define DRIVER_CLI_STAGE 64u

/* Endpoint lookup: 1 with `*handle` set when the class is registered at
 * the running protocol version, 0 otherwise. */
uint8_t driver_cli_endpoint(uint8_t class_id, uint8_t *handle);

/* One synchronous call into a resident driver with the caller's frame. */
int16_t driver_cli_call(uint8_t handle, struct kos_ipc_frame *frame);

/* Prepare a descriptor-less frame. */
void driver_cli_frame(struct kos_ipc_frame *frame, uint8_t op, uint8_t arg0,
		       uint16_t arg1);

/* Staged moves over the driver bank through the copy gate. */
int driver_cli_stage_read(uint16_t address, uint16_t length, uint8_t *dst);
int driver_cli_stage_write(uint16_t address, uint16_t length,
			    const uint8_t *src);

/* Byte-sized protocol forms: PUTC/PUT-like ops take the byte in arg0. */
int16_t driver_cli_putc(uint8_t handle, uint8_t op, uint8_t byte);
int16_t driver_cli_getc(uint8_t handle, uint8_t op);
int16_t driver_cli_poll(uint8_t handle, uint8_t op);

/* Early-console transcript writers (the diagnostic's own reporting; the
 * console *driver* is what some diagnostics exercise separately).  Hex
 * only: the nucleus links no SDCC division helpers. */
void driver_cli_puts(const char *text);
void driver_cli_putchar(uint8_t byte);
int driver_cli_write(const uint8_t *data, uint16_t length);
void driver_cli_hex8(uint8_t value);
void driver_cli_hex16(uint16_t value);

/* The console driver endpoint handle, 0 when the store booted none. */
uint8_t driver_cli_console_handle(void);

#endif /* KOS_TARGET_DRIVER_CLIENT_H */
