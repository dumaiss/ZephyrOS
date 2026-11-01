/* KonaOS SDK: the mouse (the MOUSE protocol of the HID driver) and the
 * pointer the video driver draws over the text console.
 *
 * `/dev/input/mouse` forwards the IOC controller's mouse: each read
 * returns the motion, wheel and presses since the previous read, and the
 * controller clears them, so every read is consumed.  `mouse_read` is
 * the raw report; `mouse_track` turns the deltas into a position clamped
 * to the caller's area.  A console program tracks over the 510 x 208
 * pixel text area (the cell is x / 6, y / 8) and moves the pointer there.
 *
 * Return convention: a non-negative result, or the negated native error
 * code.
 */

#ifndef KOS_SDK_MOUSE_H
#define KOS_SDK_MOUSE_H

#include "kos/types.h"
#include "kos/errno.h"

/* `flags`: a second HID interface is mounted, and its reports decode as
 * the mouse.  0 means no mouse. */
#define MOUSE_MOUNTED 0x01u
#define MOUSE_PRESENT 0x02u

/* `buttons` and `pressed`. */
#define MOUSE_LEFT    0x01u
#define MOUSE_RIGHT   0x02u
#define MOUSE_MIDDLE  0x04u

/* One read: the controller's report as the driver forwards it.  dx, dy
 * and wheel are the motion since the previous read (+dy is down), and
 * `pressed` the buttons that went down since then. */
struct mouse_report {
	uint8_t flags;
	uint8_t buttons;
	uint8_t pressed;
	int16_t dx;
	int16_t dy;
	int16_t wheel;
};

/* A tracked pointer: the position inside width x height, the wheel's
 * running total, the buttons held, the presses since the caller last
 * cleared `pressed`, and `present` (1 while a mouse decodes). */
struct mouse {
	int handle;
	int x;
	int y;
	int width;
	int height;
	int wheel;
	uint8_t buttons;
	uint8_t pressed;
	uint8_t present;
};

/* Bind `/dev/input/mouse` and return the handle, or the negated error
 * (-KOS_ENOENT when the build publishes no mouse, -KOS_EINVAL when the
 * name is not a MOUSE object). */
int mouse_open(void);

/* One report into `*r`: 0, or a negative error.  A controller without a
 * mouse, a down link and the driver's backoff after a failed exchange
 * read as an all-zero report. */
int mouse_read(int handle, struct mouse_report *r);

/* Start tracking on `handle` inside width x height (both at least 1):
 * the position at the centre, everything else clear. */
void mouse_init(struct mouse *m, int handle, int width, int height);

/* One read: adds the deltas to the position and clamps x to
 * 0..width-1 and y to 0..height-1, adds the wheel to `wheel`, ORs the
 * presses into `pressed` (the caller clears it), and sets `buttons` and
 * `present`.  Returns 1 when any of them changed, 0 when none did, or a
 * negative error. */
int mouse_track(struct mouse *m);

/* The pointer: an arrow sprite over the text console (KVIDEO_OP_POINTER),
 * its hot spot at a pixel of the 510 x 208 text area.  It stays at its
 * screen pixel when the console scrolls and stays visible while the
 * console draws; a display reset (the console reset before each start of
 * the boot program, the end of an exclusive hold on `/dev/video`) hides
 * it.  A program hides it before it exits. */
#define POINTER_WIDTH  510
#define POINTER_HEIGHT 208

/* Bind `/dev/video` (an ordinary, shared bind) and return the handle, or
 * the negated error (-KOS_EINVAL when the name is not a VIDEO object). */
int pointer_open(void);

/* Show the pointer at (x, y), x 0..509 and y 0..207: 0, -KOS_EINVAL out
 * of range, -KOS_EBUSY while a program holds the video exclusively. */
int pointer_move(int video, int x, int y);

/* Hide the pointer: 0, or -KOS_EBUSY while the video is held. */
int pointer_hide(int video);

#endif /* KOS_SDK_MOUSE_H */
