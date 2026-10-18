/* DonkeyKongKos -- KonaOS glue for the native V9958 assembly re-host.
 *
 * The game and the hot display path remain Z80 assembly.  C is deliberately
 * limited to the public KonaOS SDK boundary: files, xmem, handles, VDP
 * events, and translated keyboard input.
 */

#include "kos.h"
#include "kos/event.h"

#define BIOS_SIZE       0x2000u
#define BIOS_COPY_BASE  0x015Du
#define BIOS_COPY_SIZE  (BIOS_SIZE - BIOS_COPY_BASE)
#define CART_SIZE       0x4000u
#define DK_CRC16        0x0DC3u
#define BIOS_CRC16      0x5351u
#define VIDEO_NAME      "/dev/video"
#define DEFAULT_CART    "DK.ROM"
#define DEFAULT_BIOS    "COLECO.ROM"

extern void dk_install_hooks(void);
extern void dk_video_init(void);
extern void dk_video_shutdown(void);
extern void dk_run_game(void);
extern void dk_frame(void);

volatile unsigned char dk_in_event;
static unsigned char select_pending;
static unsigned char held_direction;
static unsigned char direction_life;
static unsigned char fire_life;
volatile unsigned char dk_quit_requested;
static unsigned char cart_token;

/* Ingest bytes without consuming the translated controller state.  This is
 * also called from the VDP event before dk_frame(): a long foreground VRAM
 * translation sets vdp_busy and skips the game NMI, but Q must still latch
 * and movement/fire must remain ready for the next real controller poll. */
static void input_pump(void)
{
	int ch;

	while (console_poll() > 0) {
		ch = getchar();
		if (ch < 0)
			break;
		switch ((unsigned char)ch) {
		case 'd': case 'D': held_direction = 0x02u; direction_life = 3u; break;
		case 'a': case 'A': held_direction = 0x08u; direction_life = 3u; break;
		case 'w': case 'W': held_direction = 0x01u; direction_life = 3u; break;
		case 's': case 'S': held_direction = 0x04u; direction_life = 3u; break;
		case ' ': case '\r': fire_life = 2u; break;
		case 'q': case 'Q': dk_quit_requested = 1u; break;
		default: break;
		}
	}
}

static unsigned int crc16_ccitt(const unsigned char *data, unsigned int length)
{
	unsigned int crc = 0xFFFFu;
	unsigned char bit;

	while (length != 0u) {
		crc ^= (unsigned int)((unsigned int)*data++ << 8);
		for (bit = 0u; bit != 8u; ++bit) {
			if ((crc & 0x8000u) != 0u)
				crc = (unsigned int)((crc << 1) ^ 0x1021u);
			else
				crc <<= 1;
		}
		--length;
	}
	return crc;
}

static int file_size_is(const char *path, unsigned long expected)
{
	struct kfs_stat record;
	int result = stat(path, &record);

	if (result < 0)
		return result;
	if (record.type != KFS_TYPE_FILE || record.size != expected)
		return -KOS_EINVAL;
	return 0;
}

/* Preserve 0100h-015Ch: that is the SDK's gate-visible data. */
static int load_bios_tail(const char *path)
{
	unsigned char *dst = (unsigned char *)BIOS_COPY_BASE;
	unsigned int done = BIOS_COPY_BASE;
	unsigned char prefix[BIOS_COPY_BASE];
	int handle;
	int count;

	handle = open(path, KFS_O_READ);
	if (handle < 0)
		return handle;
	count = read((unsigned int)handle, prefix, BIOS_COPY_BASE);
	if (count != (int)BIOS_COPY_BASE) {
		(void)close((unsigned int)handle);
		return count < 0 ? count : -KOS_EIO;
	}
	while (done < BIOS_SIZE) {
		unsigned int chunk = (unsigned int)(BIOS_SIZE - done);
		if (chunk > KOS_IPC_DESC_MAX)
			chunk = KOS_IPC_DESC_MAX;
		count = read((unsigned int)handle, dst, chunk);
		if (count <= 0) {
			(void)close((unsigned int)handle);
			return count < 0 ? count : -KOS_EIO;
		}
		dst += (unsigned int)count;
		done = (unsigned int)(done + (unsigned int)count);
	}
	(void)close((unsigned int)handle);

	/* CRC the two pieces without needing an 8 KiB primary-memory buffer. */
	{
		unsigned int crc = 0xFFFFu;
		unsigned int i;
		unsigned char bit;
		for (i = 0u; i < BIOS_COPY_BASE; ++i) {
			crc ^= (unsigned int)((unsigned int)prefix[i] << 8);
			for (bit = 0u; bit != 8u; ++bit)
				crc = (crc & 0x8000u) != 0u ?
					(unsigned int)((crc << 1) ^ 0x1021u) :
					(unsigned int)(crc << 1);
		}
		for (i = BIOS_COPY_BASE; i < BIOS_SIZE; ++i) {
			crc ^= (unsigned int)((unsigned int)*(unsigned char *)i << 8);
			for (bit = 0u; bit != 8u; ++bit)
				crc = (crc & 0x8000u) != 0u ?
					(unsigned int)((crc << 1) ^ 0x1021u) :
					(unsigned int)(crc << 1);
		}
		if (crc != BIOS_CRC16)
			return -KOS_EINVAL;
	}
	return 0;
}

static void on_frame(unsigned int count, unsigned int missed)
{
	unsigned char event_latch;

	(void)missed;
	/* Coalescing is intentional: running several expensive render frames
	 * back-to-back only makes a late game later.  OS-7 sees one NMI tick and
	 * resumes on the next physical VDP frame. */
	if (count != 0u) {
		input_pump();
		/* Event delivery clears the far-page field so the handler starts in
		 * the primary image.  Reopen DK.ROM before calling its 852Eh NMI
		 * routine, then restore the event mapping for libkos's return. */
		event_latch = aperture_open(cart_token);
		dk_in_event = 1u;
		dk_frame();
		dk_in_event = 0u;
		aperture_close(event_latch);
	}
}

/* Result bits consumed by native_poller in native.asm (Coleco raw layout):
 *   0 up, 1 right, 2 down, 3 left, 4 fire, 6 initial skill 4,
 *   7 quit.  Keyboard input has no key-up event, so motion is held for
 *   three polls; this feels natural at 60 Hz and avoids sticky keys. */
unsigned int dk_input_poll(void)
{
	unsigned char result = 0u;

	input_pump();
	if (direction_life != 0u) {
		result |= held_direction;
		--direction_life;
	}
	if (fire_life != 0u) {
		result |= 0x10u;
		--fire_life;
	}
	if (select_pending != 0u) {
		select_pending = 0u;
		result |= 0x40u;
	}
	if (dk_quit_requested != 0u)
		result |= 0x80u;
	return result;
}

static int fail(const char *what, int detail)
{
	if (detail < 0)
		printf("dkkos: %s (%d)\r\n", what, detail);
	else
		printf("dkkos: %s\r\n", what);
	return 1;
}

int main(int argc, char **argv)
{
	const char *cart_path = DEFAULT_CART;
	const char *bios_path = DEFAULT_BIOS;
	unsigned char type = 0u;
	unsigned int aperture_address = 0u;
	unsigned char saved_latch = 0u;
	int cart_handle = -1;
	int map_token;
	int video_handle = -1;
	int result;
	if (argc > 0)
		cart_path = argv[0];
	if (argc > 1)
		bios_path = argv[1];

	console_puts("DonkeyKongKos 0.4 - native V9958 G4 / sprite mode 2\r\n");
	console_puts("WASD move, SPACE jump, Q returns to KonaOS\r\n");

	result = file_size_is(cart_path, CART_SIZE);
	if (result < 0)
		return fail("DK.ROM must be the 16 KiB cartridge", result);
	result = file_size_is(bios_path, BIOS_SIZE);
	if (result < 0)
		return fail("COLECO.ROM must be the standard 8 KiB BIOS", result);

	video_handle = bind(VIDEO_NAME, KOS_BIND_EXCLUSIVE, &type);
	if (video_handle < 0)
		return fail("cannot acquire /dev/video", video_handle);
	if (type != KOS_TYPE_VIDEO) {
		(void)unbind((unsigned char)video_handle);
		return fail("/dev/video has the wrong protocol type", 0);
	}

	result = notify(video_handle, on_frame);
	if (result < 0) {
		(void)unbind((unsigned char)video_handle);
		return fail("cannot subscribe to VDP frames", result);
	}

	result = load_bios_tail(bios_path);
	if (result < 0) {
		(void)notify(video_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)video_handle);
		return fail("unsupported or unreadable COLECO.ROM", result);
	}
	cart_handle = xload(cart_path, KOS_XMEM_FLAG_MAPPABLE);
	if (cart_handle < 0) {
		(void)notify(video_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)video_handle);
		return fail("cannot load DK.ROM into extended memory", cart_handle);
	}
	map_token = xmap((unsigned int)cart_handle, &aperture_address);
	if (map_token < 0 || aperture_address != 0x8000u) {
		(void)xfree((unsigned int)cart_handle);
		(void)notify(video_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)video_handle);
		return fail("cannot map DK.ROM at the far aperture", map_token);
	}
	cart_token = (unsigned char)map_token;
	saved_latch = aperture_open(cart_token);
	if (crc16_ccitt((const unsigned char *)0x8000u, CART_SIZE) != DK_CRC16) {
		aperture_close(saved_latch);
		(void)xfree((unsigned int)cart_handle);
		(void)notify(video_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)video_handle);
		return fail("DK.ROM is not the supported 16 KiB revision", 0);
	}

	select_pending = 1u;
	dk_install_hooks();
	dk_video_init();
	dk_run_game();

	/* Disable IE0 before removing the driver's status-0 acknowledge. */
	dk_video_shutdown();
	(void)notify(video_handle, (kos_event_handler_t)0);
	aperture_close(saved_latch);
	(void)xfree((unsigned int)cart_handle);
	(void)unbind((unsigned char)video_handle);
	console_puts("dkkos: returned to KonaOS\r\n");
	return 0;
}
