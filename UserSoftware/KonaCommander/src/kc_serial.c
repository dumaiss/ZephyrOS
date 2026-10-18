#include "kos.h"
#include "kos/timer.h"

#include "kc_protocol.h"
#include "kc_serial.h"

#define KC_IDLE_TIMEOUT_COUNTS  39063ul  /* one second without a byte */
#define KC_TOTAL_TIMEOUT_COUNTS 234375ul /* six-second hard frame bound */
#define KC_RETRIES         3u
#define KC_SERIAL_CHUNK    64u

/* Public SERIAL protocol v1 operation numbers.  The standalone 0.1.0 SDK
 * exposes the generic IPC ABI but deliberately does not copy the aggregate
 * in-tree protocol header. */
#define KC_SIO_OP_WRITE    1u
#define KC_SIO_OP_READ     4u

static int serial_handle;
static int clock_handle;
static unsigned char sequence;
static unsigned char decoded[KC_MAX_DECODED];
static unsigned char tx_encoded[KC_MAX_ENCODED + 1u];
static unsigned char rx_encoded[KC_MAX_ENCODED];
static unsigned char serial_chunk[KC_SERIAL_CHUNK];

static void request_clear(struct kos_ipc_request *request)
{
	memset(request, 0, sizeof(*request));
}

static int serial_write_all(const unsigned char *data, unsigned int length)
{
	struct kos_ipc_request request;
	int result;

	request_clear(&request);
	request.frame.op = KC_SIO_OP_WRITE;
	request.frame.arg0 = 0u;
	request.frame.arg1 = length;
	request.buffer = (void *)data;
	request.length = length;
	request.direction = KOS_IPC_DESC_IN;
	result = call((unsigned char)serial_handle, &request);
	return result == (int)length ? 0 : KC_LINK_EWRITE;
}

static int serial_read_some(void)
{
	struct kos_ipc_request request;

	request_clear(&request);
	request.frame.op = KC_SIO_OP_READ;
	request.frame.arg0 = 0u;
	request.frame.arg1 = KC_SERIAL_CHUNK;
	request.buffer = serial_chunk;
	request.length = KC_SERIAL_CHUNK;
	request.direction = KOS_IPC_DESC_OUT;
	return call((unsigned char)serial_handle, &request);
}

static void serial_drain(void)
{
	unsigned char rounds = 0u;

	while (rounds++ < 8u && serial_read_some() > 0) {
		/* Bound stale-input removal so noise cannot delay a transaction. */
	}
}

int kc_link_open(void)
{
	unsigned char type = 0u;

	serial_handle = bind("/dev/serial/0", 0u, &type);
	if (serial_handle < 0) {
		return KC_LINK_EDEVICE;
	}
	if (type != KOS_TYPE_SERIAL) {
		(void)unbind((unsigned char)serial_handle);
		serial_handle = 0;
		return KC_LINK_ETYPE;
	}
	clock_handle = timer_open();
	if (clock_handle < 0) {
		(void)unbind((unsigned char)serial_handle);
		serial_handle = 0;
		return KC_LINK_ETIMER;
	}
	sequence = 0u;
	serial_drain();
	return 0;
}

void kc_link_close(void)
{
	if (clock_handle > 0) {
		(void)unbind((unsigned char)clock_handle);
		clock_handle = 0;
	}
	if (serial_handle > 0) {
		(void)unbind((unsigned char)serial_handle);
		serial_handle = 0;
	}
}

static int receive_response(const struct kc_frame *request,
			    struct kc_frame *response)
{
	uint32_t start;
	uint32_t progress;
	uint32_t now;
	unsigned int used = 0u;

	if (timer_now(clock_handle, &start) < 0) {
		return KC_LINK_ETIMER;
	}
	progress = start;
	for (;;) {
		int count = serial_read_some();
		int i;

		if (count < 0) {
			return KC_LINK_EDEVICE;
		}
		for (i = 0; i < count; ++i) {
			unsigned char byte = serial_chunk[i];

			if (byte == 0u) {
				unsigned int raw_length;
				int frame_result;

				if (used == 0u) {
					continue;
				}
				raw_length = kc_cobs_decode(rx_encoded, used, decoded,
							    sizeof(decoded));
				used = 0u;
				if (raw_length == 0u) {
					continue;
				}
				frame_result = kc_frame_unpack(decoded, raw_length,
							       response);
				if (frame_result != KC_FRAME_OK) {
					continue;
				}
				if (response->kind != KC_KIND_RESPONSE ||
				    response->command != request->command ||
				    response->sequence != request->sequence ||
				    response->flags != 0u) {
					continue;
				}
				return 0;
			}
			if (used < KC_MAX_ENCODED) {
				rx_encoded[used++] = byte;
			} else {
				used = 0u;
			}
		}
		if (timer_now(clock_handle, &now) < 0) {
			return KC_LINK_ETIMER;
		}
		if (count > 0) progress = now;
		if ((uint32_t)(now - progress) >= KC_IDLE_TIMEOUT_COUNTS ||
		    (uint32_t)(now - start) >= KC_TOTAL_TIMEOUT_COUNTS) {
			return KC_LINK_ETIMEOUT;
		}
	}
}

int kc_link_request(struct kc_frame *request, struct kc_frame *response)
{
	unsigned int raw_length;
	unsigned int wire_length;
	unsigned char attempt;
	int result = KC_LINK_ETIMEOUT;

	request->kind = KC_KIND_REQUEST;
	request->sequence = ++sequence;
	request->status = 0u;
	request->flags = 0u;
	raw_length = kc_frame_pack(request, decoded, sizeof(decoded));
	if (raw_length == 0u) {
		return KC_LINK_EFRAME;
	}
	wire_length = kc_cobs_encode(decoded, raw_length, tx_encoded,
				     KC_MAX_ENCODED);
	if (wire_length == 0u) {
		return KC_LINK_ECOBS;
	}
	tx_encoded[wire_length++] = 0u;
	serial_drain();
	for (attempt = 0u; attempt < KC_RETRIES; ++attempt) {
		result = serial_write_all(tx_encoded, wire_length);
		if (result != 0) {
			continue;
		}
		result = receive_response(request, response);
		if (result == 0) {
			return 0;
		}
	}
	return result;
}

