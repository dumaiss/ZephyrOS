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

static uint16_t buffer_end(const uint8_t *buffer)
{
	uint16_t end = SND_ZTR_ORDER_TABLE;
	unsigned int channel;

	for (channel = 0u; channel < SND_ZTR_CHANNELS; ++channel) {
		const uint8_t *entry = buffer + channel * 4u;
		uint16_t stream_end = (uint16_t)entry[2] |
			((uint16_t)entry[3] << 8);

		if (stream_end > end) {
			end = stream_end;
		}
	}
	return end;
}

int main(int argc, char **argv)
{
	kres_container container;
	kres_resource resource;
	kos_reader_t reader;
	uint16_t order;
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
	for (order = 0u; order < snd_ztr_order_count; ++order) {
		uint16_t sum = 0u;
		uint16_t end;
		const uint8_t *buffer;
		uint16_t byte;
		unsigned int waits = 0u;

		while (snd_ztr_published != (uint8_t)order) {
			(void)snd_service();
			if (++waits > 10000u) {
				printf("ztrloadkres: stalled %u\r\n",
				       (unsigned int)order);
				result = 1;
				goto done;
			}
		}
		buffer = (const uint8_t *)(uint16_t)
			snd_ztr_order_base[order & 1u];
		end = buffer_end(buffer);
		for (byte = SND_ZTR_ORDER_TABLE; byte < end; ++byte) {
			sum = checksum_byte(sum, buffer[byte]);
		}
		printf("ztrloadkres: order %u checksum %04x\r\n",
		       (unsigned int)order, (unsigned int)sum);
		snd_ztr_consumed = (uint8_t)order;
		(void)snd_service();
	}
	console_puts("ztrloadkres: PASS\r\n");
	result = 0;
done:
	snd_ztr_close();
	snd_close();
	if (resource.open != 0u) (void)kres_rclose(&resource);
	if (container.opened != 0u) (void)kres_close(&container);
	return result == 0 ? 0 : 1;
}
