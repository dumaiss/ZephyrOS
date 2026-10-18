#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kc_protocol.h"

static void test_crc(void)
{
	assert(kc_crc16((const unsigned char *)"123456789", 9u) == 0x29b1u);
}

static void test_cobs(void)
{
	unsigned char input[] = {0u, 1u, 2u, 0u, 3u, 0u};
	unsigned char encoded[32];
	unsigned char decoded[32];
	unsigned int encoded_length;
	unsigned int decoded_length;

	encoded_length = kc_cobs_encode(input, sizeof(input), encoded,
					sizeof(encoded));
	assert(encoded_length != 0u);
	decoded_length = kc_cobs_decode(encoded, encoded_length, decoded,
					sizeof(decoded));
	assert(decoded_length == sizeof(input));
	assert(memcmp(input, decoded, sizeof(input)) == 0);
}

static void test_frame(void)
{
	struct kc_frame source;
	struct kc_frame result;
	unsigned char raw[KC_MAX_DECODED];
	unsigned int length;
	unsigned int i;

	memset(&source, 0, sizeof(source));
	source.kind = KC_KIND_RESPONSE;
	source.command = KC_CMD_READ;
	source.sequence = 0x5au;
	source.payload_length = KC_MAX_PAYLOAD;
	for (i = 0u; i < KC_MAX_PAYLOAD; ++i) {
		source.payload[i] = (unsigned char)i;
	}
	length = kc_frame_pack(&source, raw, sizeof(raw));
	assert(length == KC_MAX_DECODED);
	assert(kc_frame_unpack(raw, length, &result) == KC_FRAME_OK);
	assert(result.payload_length == KC_MAX_PAYLOAD);
	assert(memcmp(source.payload, result.payload, KC_MAX_PAYLOAD) == 0);
	raw[20] ^= 1u;
	assert(kc_frame_unpack(raw, length, &result) == KC_FRAME_CRC);
	assert(kc_frame_unpack(raw, 4u, &result) == KC_FRAME_SHORT);
}

static void test_python_golden_vector(void)
{
	static const unsigned char expected[] = {
		0x4b, 0x43, 0x50, 0x31, 0x01, 0x00, 0x06, 0x2a,
		0x00, 0x00, 0x03, 0x00, 0x61, 0x00, 0x62, 0xef, 0x76
	};
	struct kc_frame frame;
	unsigned char raw[32];
	unsigned int length;

	memset(&frame, 0, sizeof(frame));
	frame.kind = KC_KIND_REQUEST;
	frame.command = KC_CMD_PING;
	frame.sequence = 0x2au;
	frame.payload_length = 3u;
	frame.payload[0] = 'a';
	frame.payload[1] = 0u;
	frame.payload[2] = 'b';
	length = kc_frame_pack(&frame, raw, sizeof(raw));
	assert(length == sizeof(expected));
	assert(memcmp(raw, expected, sizeof(expected)) == 0);
}

int main(void)
{
	test_crc();
	test_cobs();
	test_frame();
	test_python_golden_vector();
	puts("protocol tests: OK");
	return 0;
}
