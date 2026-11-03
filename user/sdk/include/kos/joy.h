/* KonaOS SDK: the game controllers (the JOY driver protocol).
 *
 * `/dev/input/joy/0` and `/dev/input/joy/1` are the board's two
 * controller latches (KOS_CLASS_JOY, type KOS_TYPE_JOY;
 * kos/protocol.h).  The IO controller decodes each USB pad (a Logitech F310
 * in DirectInput mode) into a ColecoVision controller byte and keeps it
 * in its latch; pad 0 is the first pad mounted.  A read is the byte
 * exactly as the controller set it: the ColecoVision values, active low.
 *
 * The byte, a bit low meaning pressed:
 *
 *   D0 up   D1 right   D2 down   D3 left   (the JOY_* direction masks)
 *   D6 fire: A or B (JOY_FIRE)
 *   D5:D4 high, D7 low
 *
 * An idle pad reads JOY_IDLE (7Fh), and so does an absent one: the byte
 * has no presence bit.
 *
 * The keypad shares D3:D0.  X, Y, Back and Start put a keypad code there
 * (JOY_KEY_1, JOY_KEY_2, JOY_KEY_STAR, JOY_KEY_POUND), and a USB
 * keyboard's digits, Shift+8 / keypad * and Shift+3 put the code of that
 * key on pad 0.  While a code is held it replaces the directions.  The
 * board ignores the ColecoVision mode select (OUT 80h keypad, OUT C0h
 * joystick), so there are two readings of the same byte and the program
 * chooses, as a ColecoVision game does:
 *
 *   joystick  joy_dirs(raw): the JOY_* bits, active high;
 *   keypad    joy_key(raw): the key, with JOY_KEY_FIRE (D6, low when
 *             pressed) as the keypad's second fire.
 *
 * The keypad codes (D3:D0, the ColecoVision BIOS's matrix nibbles):
 *
 *   key  code   direction reading      key  code   direction reading
 *   0    0Ah    up + down              6    0Eh    up
 *   1    0Dh    right                  7    05h    right + left
 *   2    07h    left                   8    01h    right + down + left
 *   3    0Ch    up + right             9    0Bh    down
 *   4    02h    up + down + left       *    09h    right + down
 *   5    03h    down + left            #    06h    up + left
 *
 * So 1, 2, 3, 5, 6, 9, * and # share their nibble with a direction a pad
 * can report, and 0, 4, 7 and 8 are impossible direction combinations.
 * 00h, 04h and 08h are no key, and so is 0Fh (no direction, no key).
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_JOY_H
#define KOS_SDK_JOY_H

#include "kos/types.h"

/* The latch byte's bits (active low: 0 is pressed). */
#define JOY_UP    0x01u
#define JOY_RIGHT 0x02u
#define JOY_DOWN  0x04u
#define JOY_LEFT  0x08u
#define JOY_FIRE  0x40u
#define JOY_NIBBLE 0x0Fu /* D3:D0: the directions, or a keypad code */
#define JOY_IDLE   0x7Fu /* nothing pressed, or no pad              */

/* The keypad reading: the code in D3:D0 and the second fire in D6. */
#define JOY_KEY_FIRE  0x40u
#define JOY_KEY_0     0x0Au
#define JOY_KEY_1     0x0Du /* also the pad's X     */
#define JOY_KEY_2     0x07u /* also the pad's Y     */
#define JOY_KEY_3     0x0Cu
#define JOY_KEY_4     0x02u
#define JOY_KEY_5     0x03u
#define JOY_KEY_6     0x0Eu
#define JOY_KEY_7     0x05u
#define JOY_KEY_8     0x01u
#define JOY_KEY_9     0x0Bu
#define JOY_KEY_STAR  0x09u /* also the pad's Back  */
#define JOY_KEY_POUND 0x06u /* also the pad's Start */
#define JOY_KEY_NONE  0x0Fu

/* Bind `/dev/input/joy/<pad>`, check its type and ask the driver once for
 * the pad's port.  Returns the handle, or the negated error (-KOS_ENOENT
 * for a pad the board does not have, -KOS_EINVAL when the name is another
 * type, -KOS_ENODEV or -KOS_ENOENT when the build has no joy driver). */
int joy_open(uint8_t pad);

/* Give the handle back. */
int joy_close(int joy);

/* The pad's latch byte: one IN from its port, in assembly, with no call
 * into the driver, so a game reads it every frame.  A handle joy_open did
 * not return reads JOY_IDLE. */
uint8_t joy_read(int joy);

/* The same byte through the driver (KJOY_OP_READ) into `*raw`.  0 or a
 * negative error. */
int joy_read_call(int joy, uint8_t *raw);

/* The joystick reading: JOY_UP, JOY_RIGHT, JOY_DOWN, JOY_LEFT and
 * JOY_FIRE set for what is pressed (active high).  A held keypad code
 * reads as its direction column above. */
uint8_t joy_dirs(uint8_t raw);

/* The keypad reading: '0'-'9', '*' or '#' for the code in D3:D0, or -1
 * for JOY_KEY_NONE and the codes no key has. */
int joy_key(uint8_t raw);

#endif /* KOS_SDK_JOY_H */
