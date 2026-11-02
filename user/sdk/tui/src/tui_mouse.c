/* libtui opt-in mouse.
 *
 * tui_mouse_enable() binds the mouse and the pointer and installs the
 * hook through which the key decoder reads the mouse (see tui_key.c for
 * the pacing), tui_shutdown() disables it and the widget runners take events.  Only
 * this member refers to the mouse client, so a program that never calls
 * tui_mouse_enable() links none of it.
 *
 * A read moves the pointer when the pixel position changed and makes an
 * event when a button went down or up, the wheel turned, or the cell
 * changed while a button is held (or always, with TUI_MOUSE_MOTION).  A
 * wheel turn that arrives with a press or release stays pending and comes
 * as a WHEEL event of the next read.
 *
 * The pointer is drawn over the video console, so the mouse needs that
 * console's render path: a console whose INFO (read by tui_init) does not
 * offer KCONSOLE_OP_RENDER - the serial console, or the video console
 * while its serial rescue tee is armed - refuses the enable before
 * anything is bound.
 */

#include "tui_internal.h"
#include "kos.h"
#include "kos/mouse.h"

#define TUI_CELL_PIXELS_X 6u
#define TUI_CELL_PIXELS_Y 8u

static struct mouse tui_mouse_track;
static int tui_mouse_video;
static uint8_t tui_mouse_flags;
static int tui_mouse_x;
static int tui_mouse_y;
static uint8_t tui_mouse_buttons;
static struct tui_mouse tui_mouse_event;

static int tui_mouse_poll(void);

static const struct tui_mouse_hook tui_mouse_ops = {
	tui_mouse_poll, tui_mouse_disable, tui_mouse_get
};

static uint8_t tui_mouse_cell(int pixel, uint8_t size, uint8_t limit)
{
	uint8_t cell = (uint8_t)((unsigned int)pixel / size);

	return cell < limit ? cell : (uint8_t)(limit - 1u);
}

/* One read: 1 when it made an event, else 0 (a read error is no event). */
static int tui_mouse_poll(void)
{
	struct mouse *m = &tui_mouse_track;
	uint8_t col;
	uint8_t row;
	uint8_t pressed;
	uint8_t released;
	uint8_t kind;
	uint8_t moved;
	int result = mouse_track(m);

	/* A turn left pending by an earlier press or release is a change. */
	if (result < 0 || (result == 0 && m->wheel == 0)) {
		return 0;
	}
	if (m->x != tui_mouse_x || m->y != tui_mouse_y) {
		tui_mouse_x = m->x;
		tui_mouse_y = m->y;
		(void)pointer_move(tui_mouse_video, m->x, m->y);
	}
	col = tui_mouse_cell(m->x, TUI_CELL_PIXELS_X, tui_state.screen.cols);
	row = tui_mouse_cell(m->y, TUI_CELL_PIXELS_Y, tui_state.screen.rows);
	pressed = m->pressed;
	m->pressed = 0u;
	/* A press and its release inside one read still count as both. */
	released = (uint8_t)((tui_mouse_buttons | pressed) &
			     (uint8_t)~m->buttons);
	tui_mouse_buttons = m->buttons;
	moved = (uint8_t)(col != tui_mouse_event.col ||
			  row != tui_mouse_event.row);
	if (pressed != 0u) {
		kind = TUI_MOUSE_PRESS;
	} else if (released != 0u) {
		kind = TUI_MOUSE_RELEASE;
	} else if (m->wheel != 0) {
		kind = TUI_MOUSE_WHEEL;
	} else if (moved != 0u &&
		   (m->buttons != 0u ||
		    (tui_mouse_flags & TUI_MOUSE_MOTION) != 0u)) {
		kind = TUI_MOUSE_MOVE;
	} else {
		kind = TUI_MOUSE_NONE;
	}
	tui_mouse_event.col = col;
	tui_mouse_event.row = row;
	if (kind == TUI_MOUSE_NONE) {
		return 0;
	}
	tui_mouse_event.kind = kind;
	tui_mouse_event.buttons = m->buttons;
	tui_mouse_event.pressed = pressed;
	tui_mouse_event.released = released;
	if (kind == TUI_MOUSE_WHEEL) {
		tui_mouse_event.wheel = (int16_t)m->wheel;
		m->wheel = 0;
	} else {
		tui_mouse_event.wheel = 0;
	}
	return 1;
}

int tui_mouse_enable(uint8_t flags)
{
	int mouse;
	int video;
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	if ((tui_state.console_flags & KCONSOLE_INFO_RENDER) == 0u) {
		return -KOS_ENOTSUP;
	}
	tui_mouse_flags = flags;
	if (tui_mouse_hook != (const struct tui_mouse_hook *)0) {
		return 0;
	}
	mouse = mouse_open();
	if (mouse < 0) {
		return mouse;
	}
	video = pointer_open();
	if (video < 0) {
		(void)unbind((unsigned char)mouse);
		return video;
	}
	mouse_init(&tui_mouse_track, mouse, POINTER_WIDTH, POINTER_HEIGHT);
	tui_mouse_x = tui_mouse_track.x;
	tui_mouse_y = tui_mouse_track.y;
	result = pointer_move(video, tui_mouse_x, tui_mouse_y);
	if (result < 0) {
		(void)unbind((unsigned char)video);
		(void)unbind((unsigned char)mouse);
		return result;
	}
	tui_mouse_video = video;
	tui_mouse_buttons = 0u;
	tui_mouse_event.kind = TUI_MOUSE_NONE;
	tui_mouse_event.col = tui_mouse_cell(tui_mouse_x, TUI_CELL_PIXELS_X,
					     tui_state.screen.cols);
	tui_mouse_event.row = tui_mouse_cell(tui_mouse_y, TUI_CELL_PIXELS_Y,
					     tui_state.screen.rows);
	tui_mouse_event.buttons = 0u;
	tui_mouse_event.pressed = 0u;
	tui_mouse_event.released = 0u;
	tui_mouse_event.wheel = 0;
	tui_mouse_hook = &tui_mouse_ops;
	return 0;
}

void tui_mouse_disable(void)
{
	if (tui_mouse_hook == (const struct tui_mouse_hook *)0) {
		return;
	}
	tui_mouse_hook = (const struct tui_mouse_hook *)0;
	(void)pointer_hide(tui_mouse_video);
	(void)unbind((unsigned char)tui_mouse_video);
	(void)unbind((unsigned char)tui_mouse_track.handle);
}

int tui_mouse_get(struct tui_mouse *ev)
{
	const struct tui_rect *window = &tui_state.window;
	uint8_t col;
	uint8_t row;

	if (ev == (struct tui_mouse *)0 ||
	    tui_mouse_hook == (const struct tui_mouse_hook *)0) {
		return -KOS_EINVAL;
	}
	*ev = tui_mouse_event;
	col = tui_mouse_event.col;
	row = tui_mouse_event.row;
	if (col >= window->x && row >= window->y &&
	    (uint8_t)(col - window->x) < window->w &&
	    (uint8_t)(row - window->y) < window->h) {
		ev->wcol = (int8_t)(col - window->x);
		ev->wrow = (int8_t)(row - window->y);
	} else {
		ev->wcol = -1;
		ev->wrow = -1;
	}
	return 0;
}

int tui_mouse_hit(const struct tui_mouse *ev, const struct tui_rect *rect)
{
	uint8_t col;
	uint8_t row;

	if (ev == (const struct tui_mouse *)0 ||
	    rect == (const struct tui_rect *)0 ||
	    ev->wcol < 0 || ev->wrow < 0) {
		return 0;
	}
	col = (uint8_t)ev->wcol;
	row = (uint8_t)ev->wrow;
	return (uint8_t)(col - rect->x) < rect->w && col >= rect->x &&
	       (uint8_t)(row - rect->y) < rect->h && row >= rect->y;
}
