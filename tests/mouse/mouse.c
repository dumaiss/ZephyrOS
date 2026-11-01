/* KonaOS mouse outcome program: `/system/mouse.exe`.
 *
 * Two uses:
 *
 *   `mouse`        the owner's check with a dongle: the pointer follows
 *                  the mouse over the 510 x 208 text area, starting at
 *                  the centre and stopping at the edges, and every press
 *                  prints the text cell under it, until a key is pressed;
 *                  the pointer is hidden at the end;
 *
 *   `mouse stage`  the acceptance replay's run: the bind and its type,
 *                  then STAGE_READS tracks, one line each with the
 *                  track's result and the whole tracked state, the
 *                  pointer following each.  The presses are cleared after
 *                  every second line, so a press shows on the line of its
 *                  read and the next one.  Then the pointer checks: a
 *                  sweep over every row, the range refusals, the cursor
 *                  hidden under a shown pointer, console scrolling and two
 *                  full-screen renders (pointer shown, then hidden), the
 *                  refusal while the video is held exclusively and the
 *                  resets.  The replay scripts the controller, stops at
 *                  stage_mark and judges the lines and the sprite table.
 *
 * Every line is `mouse: ...`; the exit status is 0 unless the mouse or
 * the pointer could not be opened or a read failed.
 */

#include "kos.h"
#include "kos/mouse.h"
#include "kos/protocol.h"
#include "kos/console_render.h"

#define STAGE_TAIL  "stage"
#define STAGE_READS 18u
#define SCROLL_LINES 30u

/* The full-screen render: a scroll of the whole grid, one 85-cell TEXT
 * record per row and the final cursor STATE. */
#define GRID_COLUMNS 85u
#define GRID_ROWS    26u
#define RENDER_LENGTH (KCONSOLE_RENDER_HEADER_SIZE + \
		       KCONSOLE_RENDER_SCROLL_SIZE + \
		       GRID_ROWS * (KCONSOLE_RENDER_SPAN_BASE + GRID_COLUMNS) + \
		       KCONSOLE_RENDER_STATE_SIZE)

static struct mouse pointer;
static int video;
static uint8_t stream[RENDER_LENGTH];

/* The replay's checkpoints: it stops at stage_mark's entry and reads
 * stage_step. */
volatile unsigned char stage_step;

void stage_mark(void)
{
}

static void mark(unsigned char step)
{
	(void)console_flush();
	stage_step = step;
	stage_mark();
}

static int same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1;
		}
		++a;
		++b;
	}
	return 0;
}

static void show(const char *lead)
{
	printf("mouse: %s%d,%d buttons %u pressed %u wheel %d present %u\r\n",
	       lead, pointer.x, pointer.y, (unsigned int)pointer.buttons,
	       (unsigned int)pointer.pressed, pointer.wheel,
	       (unsigned int)pointer.present);
}

static int open_pointer(void)
{
	int handle;

	video = pointer_open();
	if (video < 0) {
		printf("mouse: pointer open failed %d\r\n", video);
		return video;
	}
	handle = mouse_open();
	if (handle < 0) {
		printf("mouse: open failed %d\r\n", handle);
		return handle;
	}
	mouse_init(&pointer, handle, POINTER_WIDTH, POINTER_HEIGHT);
	return handle;
}

static void build_render(char fill)
{
	uint8_t *at = stream;
	uint8_t row;
	uint8_t column;

	*at++ = KCONSOLE_RENDER_VERSION;
	*at++ = 0u;
	*at++ = (uint8_t)(RENDER_LENGTH & 0xFFu);
	*at++ = (uint8_t)(RENDER_LENGTH >> 8);
	*at++ = KCONSOLE_RENDER_SCROLL;
	*at++ = KCONSOLE_RENDER_SCROLL_SIZE;
	*at++ = 0u;
	*at++ = GRID_ROWS;
	*at++ = KCONSOLE_SCROLL_UP;
	*at++ = 0u;
	for (row = 0u; row < GRID_ROWS; ++row) {
		*at++ = KCONSOLE_RENDER_TEXT;
		*at++ = KCONSOLE_RENDER_SPAN_BASE + GRID_COLUMNS;
		*at++ = 0u;
		*at++ = row;
		*at++ = 0u;
		*at++ = GRID_COLUMNS;
		for (column = 0u; column < GRID_COLUMNS; ++column) {
			*at++ = (uint8_t)fill;
		}
	}
	*at++ = KCONSOLE_RENDER_STATE;
	*at++ = KCONSOLE_RENDER_STATE_SIZE;
	*at++ = 0u;
	*at++ = GRID_ROWS - 1u;
	*at++ = 0u;
	*at = 1u;
}

static int video_reset(void)
{
	struct kos_ipc_request req;

	req.frame.op = KVIDEO_OP_RESET;
	req.frame.flags = 0u;
	req.frame.desc = KOS_IPC_DESC_NONE;
	req.frame.arg0 = 0u;
	req.frame.arg1 = 0u;
	req.buffer = (void *)0;
	req.length = 0u;
	req.direction = KOS_IPC_DESC_IN;
	return call((unsigned char)video, &req);
}

/* The pointer checks after the reads; the pointer starts where the
 * last read left it. */
static void pointer_stage(void)
{
	unsigned char type = 0u;
	unsigned int failures = 0u;
	unsigned int line;
	int y;
	int held;
	int busy;
	int a;
	int b;
	int c;

	mark(1);
	for (y = 0; y < POINTER_HEIGHT; ++y) {
		if (pointer_move(video, y * 2, y) != 0) {
			++failures;
		}
	}
	mark(2);
	printf("mouse: pointer sweep %u\r\n", failures);
	a = pointer_move(video, POINTER_WIDTH, 0);
	b = pointer_move(video, 0, POINTER_HEIGHT);
	c = pointer_move(video, -1, 0);
	printf("mouse: pointer range %d %d %d\r\n", a, b, c);
	(void)pointer_move(video, 100, 50);
	mark(3);
	console_puts("\033[?25l");
	mark(4);
	console_puts("\033[?25h");
	for (line = 0u; line < SCROLL_LINES; ++line) {
		printf("mouse: scroll %u\r\n", line);
	}
	mark(5);
	build_render('#');
	a = console_render(stream, RENDER_LENGTH);
	mark(6);
	b = pointer_hide(video);
	mark(7);
	build_render('.');
	c = console_render(stream, RENDER_LENGTH);
	mark(8);
	printf("mouse: pointer render %d hide %d render %d\r\n", a, b, c);

	(void)pointer_move(video, 30, 40);
	held = bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
	mark(9);
	busy = pointer_move(video, 60, 60);
	mark(10);
	if (held >= 0) {
		(void)unbind((unsigned char)held);
	}
	mark(11);
	printf("mouse: pointer held %d busy %d\r\n", held >= 0 ? 0 : held,
	       busy);
	(void)pointer_move(video, 30, 40);
	mark(12);
	a = video_reset();
	mark(13);
	printf("mouse: pointer reset %d\r\n", a);
}

/* `mouse stage`: the scripted reads, one line each, then the pointer. */
static int mouse_stage(void)
{
	char lead[12];
	unsigned int read;
	int result;
	int moved;

	unsigned char type = 0u;
	int handle = bind("/dev/input/mouse", 0u, &type);

	/* The type the name resolves to, before the client's own check. */
	printf("mouse: bind %d type %u\r\n", handle >= 0 ? 0 : handle,
	       (unsigned int)type);
	if (handle >= 0) {
		(void)unbind((unsigned char)handle);
	}
	if (open_pointer() < 0) {
		return 1;
	}
	show("start ");
	for (read = 1u; read <= STAGE_READS; ++read) {
		result = mouse_track(&pointer);
		if (result < 0) {
			printf("mouse: read %u failed %d\r\n", read, result);
			return 1;
		}
		moved = pointer_move(video, pointer.x, pointer.y);
		if (moved != 0) {
			printf("mouse: pointer %u failed %d\r\n", read, moved);
			return 1;
		}
		snprintf(lead, sizeof(lead), "%u %d at ", read, result);
		show(lead);
		if ((read & 1u) == 0u) {
			pointer.pressed = 0u;
		}
	}
	pointer_stage();
	console_puts("mouse: stage done\r\n");
	return 0;
}

/* `mouse`: the pointer follows the mouse until a key. */
static int mouse_watch(void)
{
	char key;
	int result;
	uint8_t present = 0u;

	if (open_pointer() < 0) {
		return 1;
	}
	console_puts("mouse: move and click; any key ends\r\n");
	(void)pointer_move(video, pointer.x, pointer.y);
	for (;;) {
		result = mouse_track(&pointer);
		if (result < 0) {
			printf("mouse: read failed %d\r\n", result);
			(void)pointer_hide(video);
			return 1;
		}
		if (result != 0) {
			(void)pointer_move(video, pointer.x, pointer.y);
			if (pointer.present != present) {
				present = pointer.present;
				printf("mouse: present %u\r\n",
				       (unsigned int)present);
			}
			if (pointer.pressed != 0u) {
				printf("mouse: cell %d,%d pressed %u\r\n",
				       pointer.x / 6, pointer.y / 8,
				       (unsigned int)pointer.pressed);
				pointer.pressed = 0u;
			}
		}
		if (console_read(&key, 1u) > 0) {
			(void)pointer_hide(video);
			return 0;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 1 && argv != (char **)0 && same(argv[0], STAGE_TAIL)) {
		return mouse_stage();
	}
	return mouse_watch();
}
