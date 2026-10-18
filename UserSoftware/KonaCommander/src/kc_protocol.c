#include "kc_protocol.h"

unsigned int kc_get_u16(const unsigned char *data)
{
	return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
}

unsigned long kc_get_u32(const unsigned char *data)
{
	return (unsigned long)data[0] |
	       ((unsigned long)data[1] << 8) |
	       ((unsigned long)data[2] << 16) |
	       ((unsigned long)data[3] << 24);
}

void kc_put_u16(unsigned char *data, unsigned int value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
}

void kc_put_u32(unsigned char *data, unsigned long value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
	data[2] = (unsigned char)(value >> 16);
	data[3] = (unsigned char)(value >> 24);
}

unsigned int kc_crc16(const unsigned char *data, unsigned int length)
{
	unsigned int crc = 0xffffu;
	unsigned int i;

	while (length-- != 0u) {
		crc ^= (unsigned int)*data++ << 8;
		for (i = 0u; i < 8u; ++i) {
			crc = (crc & 0x8000u) != 0u
				? (unsigned int)((crc << 1) ^ 0x1021u)
				: (unsigned int)(crc << 1);
			crc &= 0xffffu;
		}
	}
	return crc;
}

unsigned int kc_cobs_encode(const unsigned char *input, unsigned int length,
			    unsigned char *output, unsigned int capacity)
{
	unsigned int read_index = 0u;
	unsigned int write_index = 1u;
	unsigned int code_index = 0u;
	unsigned char code = 1u;

	if (capacity == 0u) {
		return 0u;
	}
	while (read_index < length) {
		if (input[read_index] == 0u) {
			if (code_index >= capacity) {
				return 0u;
			}
			output[code_index] = code;
			code = 1u;
			code_index = write_index++;
			if (write_index > capacity) {
				return 0u;
			}
			++read_index;
		} else {
			if (write_index >= capacity) {
				return 0u;
			}
			output[write_index++] = input[read_index++];
			++code;
			if (code == 0xffu) {
				output[code_index] = code;
				code = 1u;
				code_index = write_index++;
				if (write_index > capacity) {
					return 0u;
				}
			}
		}
	}
	if (code_index >= capacity) {
		return 0u;
	}
	output[code_index] = code;
	return write_index;
}

unsigned int kc_cobs_decode(const unsigned char *input, unsigned int length,
			    unsigned char *output, unsigned int capacity)
{
	unsigned int read_index = 0u;
	unsigned int write_index = 0u;

	while (read_index < length) {
		unsigned int code = input[read_index++];
		unsigned int copy;

		if (code == 0u || read_index + code - 1u > length) {
			return 0u;
		}
		copy = code - 1u;
		if (write_index + copy > capacity) {
			return 0u;
		}
		while (copy-- != 0u) {
			output[write_index++] = input[read_index++];
		}
		if (code != 0xffu && read_index < length) {
			if (write_index >= capacity) {
				return 0u;
			}
			output[write_index++] = 0u;
		}
	}
	return write_index;
}

unsigned int kc_frame_pack(const struct kc_frame *frame,
			   unsigned char *output, unsigned int capacity)
{
	unsigned int length;
	unsigned int i;
	unsigned int crc;

	if (frame->payload_length > KC_MAX_PAYLOAD) {
		return 0u;
	}
	length = KC_HEADER_SIZE + frame->payload_length + KC_CRC_SIZE;
	if (capacity < length) {
		return 0u;
	}
	output[0] = 'K'; output[1] = 'C'; output[2] = 'P'; output[3] = '1';
	output[4] = KC_VERSION;
	output[5] = frame->kind;
	output[6] = frame->command;
	output[7] = frame->sequence;
	output[8] = frame->status;
	output[9] = frame->flags;
	kc_put_u16(output + 10u, frame->payload_length);
	for (i = 0u; i < frame->payload_length; ++i) {
		output[KC_HEADER_SIZE + i] = frame->payload[i];
	}
	crc = kc_crc16(output, (unsigned int)(length - KC_CRC_SIZE));
	kc_put_u16(output + length - KC_CRC_SIZE, crc);
	return length;
}

int kc_frame_unpack(const unsigned char *input, unsigned int length,
		    struct kc_frame *frame)
{
	unsigned int payload_length;
	unsigned int expected;
	unsigned int i;

	if (length < KC_HEADER_SIZE + KC_CRC_SIZE) {
		return KC_FRAME_SHORT;
	}
	if (input[0] != 'K' || input[1] != 'C' ||
	    input[2] != 'P' || input[3] != '1') {
		return KC_FRAME_MAGIC;
	}
	if (input[4] != KC_VERSION) {
		return KC_FRAME_VERSION;
	}
	payload_length = kc_get_u16(input + 10u);
	if (payload_length > KC_MAX_PAYLOAD ||
	    length != KC_HEADER_SIZE + payload_length + KC_CRC_SIZE) {
		return KC_FRAME_LENGTH;
	}
	expected = kc_get_u16(input + length - KC_CRC_SIZE);
	if (kc_crc16(input, (unsigned int)(length - KC_CRC_SIZE)) != expected) {
		return KC_FRAME_CRC;
	}
	frame->kind = input[5];
	frame->command = input[6];
	frame->sequence = input[7];
	frame->status = input[8];
	frame->flags = input[9];
	frame->payload_length = payload_length;
	for (i = 0u; i < payload_length; ++i) {
		frame->payload[i] = input[KC_HEADER_SIZE + i];
	}
	return KC_FRAME_OK;
}

