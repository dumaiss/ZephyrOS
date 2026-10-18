/* KonaOS vertical-blank event outcome program (card program, run from the
 * production shell by tests/test_vblank.py).
 *
 * The program takes the VDP exclusively, subscribes to its frame event,
 * turns the V9958's frame interrupt on itself (IE0 in R#1, as a ColecoVision
 * program does) and then spins without a single system call: every event
 * is delivered at the interrupt's own return into this loop.  After five
 * frames it turns the interrupt off, gives the VDP back (the console draws
 * again) and reports.
 */

#include "kos.h"
#include "kos/event.h"

#define VBLANK_FRAMES 5u
#define VBLANK_SPIN   60000u

/* The V9958 command port: R#1 is written as value, then 80h | 1. */
__sfr __at 0xA1 vblank_vdp;

static volatile unsigned int vblank_frames;
static volatile unsigned int vblank_calls;

static void vblank_handler(unsigned int count, unsigned int missed)
{
	(void)missed;
	vblank_frames += count;
	++vblank_calls;
}

/* R#1 as one pair, masked: the event's acknowledge reads status, which
 * resets the command port's two-byte latch. */
static void vblank_r1(unsigned char value)
{
	__asm__("di");
	vblank_vdp = value;
	vblank_vdp = 0x81u;
	__asm__("ei");
}

int main(void)
{
	unsigned char type = 0u;
	unsigned int spin = 0u;
	unsigned int frames;
	int video;
	int result;

	video = bind("/dev/video", KOS_BIND_EXCLUSIVE, &type);
	if (video < 0) {
		printf("vblank: FAIL bind %d\r\n", video);
		return 1;
	}
	result = notify((unsigned int)video, vblank_handler);
	if (result < 0) {
		(void)unbind((unsigned char)video);
		printf("vblank: FAIL notify %d\r\n", result);
		return 1;
	}
	vblank_r1(0x60u); /* display on, frame interrupt on */
	while (vblank_frames < VBLANK_FRAMES && spin < VBLANK_SPIN) {
		++spin; /* no system call in here */
	}
	vblank_r1(0x40u); /* frame interrupt off */
	frames = vblank_frames;
	(void)notify((unsigned int)video, (kos_event_handler_t)0);
	(void)unbind((unsigned char)video);
	printf("vblank: %u frames in %u calls\r\n", frames, vblank_calls);
	if (frames < VBLANK_FRAMES) {
		printf("vblank: FAIL frames\r\n");
		return 1;
	}
	printf("vblank: PASS\r\n");
	return 0;
}
