/* KonaOS libkos: the two readings of a game controller's latch byte.
 *
 * Pure functions of the byte (kos/joy.h): the joystick reading and the
 * keypad reading.  They include nothing but kos/joy.h, so the host test
 * compiles this file as it is (tests/test_joy.py).
 */

#include "kos/joy.h"

/* The keypad key of each D3:D0 code, 0 for none: the IO controller's
 * table (Zephyr-80 IOController src/ioc_hid.c, coleco_keypad_nibble),
 * indexed by the code. */
static const char joy_keys[16] = {
	0, '8', '4', '5', 0, '7', '#', '2',
	0, '*', '0', '9', '3', '1', '6', 0
};

uint8_t joy_dirs(uint8_t raw)
{
	return (uint8_t)(~raw & (JOY_UP | JOY_RIGHT | JOY_DOWN | JOY_LEFT |
				 JOY_FIRE));
}

int joy_key(uint8_t raw)
{
	char key = joy_keys[raw & JOY_NIBBLE];

	return key != 0 ? (int)key : -1;
}
