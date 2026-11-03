/* KonaOS SDK: the serial port, held for direct I/O (the SERIAL driver
 * protocol).
 *
 * `/dev/serial/0` is SIO0/B (KOS_CLASS_SERIAL, type KOS_TYPE_SERIAL;
 * kos/protocol.h), 115,200 baud 8N1.  A byte arrives every 87 µs, and a
 * call into the driver costs more than that, so a program that moves a
 * stream holds the port and polls it itself:
 *
 *   serial_hold      bind the port exclusively and learn its ports from
 *                    the driver; the driver then touches none of them,
 *                    and the console's serial rescue and tee pause;
 *   serial_rts       raise RTS while the program is ready to receive and
 *                    drop it around anything slow, so a peer on RTS/CTS
 *                    flow control holds its output meanwhile (the receive
 *                    FIFO is 3 bytes);
 *   serial_getc_wait / serial_putc / serial_write
 *                    the bytes, by RR0 and the data port, with no OS call
 *                    per byte;
 *   serial_release   give the port back (the program's exit does it too):
 *                    the driver rewrites the channel's setup, with RTS
 *                    asserted, and drains the receiver.
 *
 * The byte functions are for the holder only; before serial_hold or
 * after serial_release they do nothing (getc returns -1, putc and write
 * report nothing sent).
 *
 * Return convention: a non-negative result, or the negated native error
 * code, except where a function says otherwise.
 */

#ifndef KOS_SDK_SERIAL_H
#define KOS_SDK_SERIAL_H

#include "kos/types.h"
#include "kos/errno.h"

/* serial_getc_wait's timeout unit: one tick is 10 ms of polling at
 * 10 MHz (interrupt service taken meanwhile lengthens it). */
#define SERIAL_TICKS_PER_SECOND 100u

/* Hold `/dev/serial/0`: 0, or the negated error (-KOS_EBUSY while another
 * program, or this one, holds it; -KOS_ENOENT / -KOS_ENODEV without the
 * serial driver; -KOS_ENOTSUP when the driver offers no direct I/O). */
int serial_hold(void);

/* End the hold: 0, or the negated error (-KOS_EINVAL when not held). */
int serial_release(void);

/* WR5 with RTS raised (`on` non-zero) or dropped; DTR and the
 * transmitter stay enabled. */
void serial_rts(unsigned char on);

/* The next received byte (0-255), waiting up to `ticks` ticks for it;
 * -1 when none arrived.  0 ticks checks once. */
int serial_getc_wait(unsigned int ticks);

/* Send one byte once the transmitter is empty: 0, or -1 when it never
 * empties (a bounded wait of about a third of a second). */
int serial_putc(unsigned char ch);

/* Send `length` bytes the same way: the count sent, short only when the
 * transmitter stopped emptying. */
int serial_write(const void *buffer, unsigned int length);

#endif /* KOS_SDK_SERIAL_H */
