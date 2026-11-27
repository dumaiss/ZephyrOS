/* Simulator performance probe: ordinary SDK calls, controlled by the host. */
#include "kos.h"
#include "kos/sound.h"

unsigned int perf_size, perf_items, perf_result;
unsigned int perf_address, perf_length;
unsigned int perf_object, perf_token, perf_far_address;
int perf_sound;
FILE *perf_stream;
static char perf_scratch[512];

void perf_ready(void) { __asm nop __endasm; }
void perf_open_read(void) { perf_stream = fopen("/fs/SMALL.BIN", "rb"); }
void perf_open_write(void) { perf_stream = fopen("/fs/WRITE.BIN", "wb"); }
/* Cache states a read starts from: a writable open drops the whole cache
 * ("cold"); a full read of SMALL.BIN then a block of OTHER.BIN leaves
 * every SMALL block cached with the ring on the other file ("warm"). */
void perf_prepare_cold(void)
{
	FILE *file = fopen("/fs/WRITE.BIN", "wb");

	if (file != (FILE *)0) {
		(void)fclose(file);
	}
}
void perf_prepare_warm(void)
{
	FILE *file = fopen("/fs/SMALL.BIN", "rb");

	while (fread(perf_scratch, 1u, 512u, file) == 512u) {
	}
	(void)fclose(file);
	file = fopen("/fs/OTHER.BIN", "rb");
	(void)fread(perf_scratch, 1u, 512u, file);
	(void)fclose(file);
}
void perf_close(void) { perf_result = fclose(perf_stream); }
void perf_read(void)
{
	perf_result = fread((void *)perf_address, perf_size, perf_items,
			    perf_stream);
}
void perf_write(void)
{
	perf_result = fwrite((const void *)perf_address, perf_size, perf_items,
			     perf_stream);
}
void perf_fputs(void)
{
	perf_result = (unsigned int)fputs((const char *)perf_address,
					  perf_stream);
}
void perf_console(void)
{
	perf_result = console_puts((const char *)perf_address);
}
void perf_flush(void) { perf_result = console_flush(); }
void perf_sound_write(void)
{
	perf_result = sound_write(perf_sound,
		(const struct ksound_write *)perf_address, perf_length);
}
void perf_far_open(void) { (void)aperture_open((unsigned char)perf_token); }
void perf_far_close(void) { aperture_close(0u); }

int main(void)
{
	perf_sound = sound_open(0u);
	perf_object = (unsigned int)xalloc(16384u, KOS_XMEM_FLAG_MAPPABLE);
	perf_token = (unsigned int)xmap(perf_object, &perf_far_address);
	perf_ready();
	for (;;) { __asm halt __endasm; }
}
