#ifndef KC_PROTOCOL_H
#define KC_PROTOCOL_H

#define KC_VERSION             1u
#define KC_KIND_REQUEST        0u
#define KC_KIND_RESPONSE       1u

#define KC_CMD_HELLO           0x01u
#define KC_CMD_LIST            0x02u
#define KC_CMD_OPEN            0x03u
#define KC_CMD_READ            0x04u
#define KC_CMD_CLOSE           0x05u
#define KC_CMD_PING            0x06u
#define KC_CMD_CREATE          0x07u
#define KC_CMD_WRITE           0x08u
#define KC_CMD_COMMIT          0x09u
#define KC_CMD_ABORT           0x0au

#define KC_CAP_WRITE           0x01u

#define KC_STATUS_OK           0u
#define KC_STATUS_BAD_REQUEST  1u
#define KC_STATUS_NOT_FOUND    2u
#define KC_STATUS_NOT_DIR      3u
#define KC_STATUS_NOT_FILE     4u
#define KC_STATUS_DENIED       5u
#define KC_STATUS_BAD_HANDLE   6u
#define KC_STATUS_NO_HANDLES   7u
#define KC_STATUS_IO_ERROR     8u
#define KC_STATUS_UNSUPPORTED  9u
#define KC_STATUS_TOO_LARGE    10u

#define KC_MAX_PAYLOAD         768u
#define KC_HEADER_SIZE         12u
#define KC_CRC_SIZE            2u
#define KC_MAX_DECODED         (KC_HEADER_SIZE + KC_MAX_PAYLOAD + KC_CRC_SIZE)
#define KC_MAX_ENCODED         800u
#define KC_MAX_PATH            191u
#define KC_MAX_NAME            63u

#define KC_ENTRY_DIRECTORY     0x01u

#define KC_FRAME_OK            0
#define KC_FRAME_SHORT        -1
#define KC_FRAME_MAGIC        -2
#define KC_FRAME_VERSION      -3
#define KC_FRAME_LENGTH       -4
#define KC_FRAME_CRC          -5

struct kc_frame {
	unsigned char kind;
	unsigned char command;
	unsigned char sequence;
	unsigned char status;
	unsigned char flags;
	unsigned int payload_length;
	unsigned char payload[KC_MAX_PAYLOAD];
};

unsigned int kc_get_u16(const unsigned char *data);
unsigned long kc_get_u32(const unsigned char *data);
void kc_put_u16(unsigned char *data, unsigned int value);
void kc_put_u32(unsigned char *data, unsigned long value);
unsigned int kc_crc16(const unsigned char *data, unsigned int length);
unsigned int kc_cobs_encode(const unsigned char *input, unsigned int length,
			    unsigned char *output, unsigned int capacity);
unsigned int kc_cobs_decode(const unsigned char *input, unsigned int length,
			    unsigned char *output, unsigned int capacity);
unsigned int kc_frame_pack(const struct kc_frame *frame,
			   unsigned char *output, unsigned int capacity);
int kc_frame_unpack(const unsigned char *input, unsigned int length,
		    struct kc_frame *frame);

#endif

