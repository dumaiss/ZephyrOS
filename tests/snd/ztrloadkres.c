/* Minimal KRES-backed ZTR producer proof.  Kept separate from sndtest so
 * ordinary sound programs do not pull libkres into their link. */

#include "kos.h"
#include "kos/kres.h"
#include "kos/kres_konaos.h"
#include "kos/reader.h"
#include "kos/snd.h"
#include "snd_private.h"

static unsigned char kres_context;

static uint16_t checksum_byte(uint16_t sum, uint8_t byte)
{
	return (uint16_t)(((sum << 1) | (sum >> 15)) + byte);
}

int main(int argc, char **argv)
{
	kres_container container;
	kres_resource resource;
	kos_reader_t reader;
	uint16_t order = 0u;
	uint16_t sum = 0u;
	int result;

	if (argc != 2) {
		console_puts("usage: ztrloadkres FILE.KPR\r\n");
		return 1;
	}
	memset(&container, 0, sizeof(container));
	memset(&resource, 0, sizeof(resource));
	kres_install(&kres_io_konaos_read, &kres_cache_xmem, &kres_context);
	result = kres_open(&container, argv[0], KRES_OPEN_READ);
	if (result == 0) {
		result = kres_get(&container, argv[1], &resource,
				  KRES_OPEN_READ);
	}
	if (result == 0) result = kos_reader_from_kres(&reader, &resource);
	if (result == 0) result = snd_open();
	if (result == 0) result = snd_ztr_open(&reader);
	if (result == 0) result = snd_ztr_prepare();
	if (result != 0 && result != SND_NEED_MORE) {
		printf("ztrloadkres: open %d\r\n", result);
		goto done;
	}
	for (;;) {
		uint8_t head = snd_ztr_head;

		while (head != snd_ztr_tail) {
			const uint8_t *record =
				(const uint8_t *)snd_ztr_queue_addrs[head];
			unsigned int byte;

			if (record[0] != (uint8_t)order) {
				printf("ztrloadkres: order %u checksum %04x\r\n",
				       (unsigned int)order, (unsigned int)sum);
				order = record[0];
				sum = 0u;
			}
			for (byte = 0u; byte < SND_ZTR_ROW_BYTES; ++byte) {
				sum = checksum_byte(sum, record[byte]);
			}
			if (++head == SND_ZTR_QUEUE_ROWS) head = 0u;
			snd_ztr_head = head;
		}
		{
			uint8_t before = snd_ztr_tail;

			(void)snd_service();
			if (snd_ztr_tail == before) break;
		}
	}
	printf("ztrloadkres: order %u checksum %04x\r\n",
	       (unsigned int)order, (unsigned int)sum);
	console_puts("ztrloadkres: PASS\r\n");
	result = 0;
done:
	snd_ztr_close();
	snd_close();
	if (resource.open != 0u) (void)kres_rclose(&resource);
	if (container.opened != 0u) (void)kres_close(&container);
	return result == 0 ? 0 : 1;
}
