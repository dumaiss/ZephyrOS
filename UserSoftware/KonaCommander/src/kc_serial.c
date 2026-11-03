#include "kos.h"
#include "kos/serial.h"
#include "kos/timer.h"

#include "kc_protocol.h"
#include "kc_serial.h"

/* The link holds SIO0/B for the whole session and moves every byte
 * itself (kos/serial.h).  A byte arrives every 87 us, so nothing slow may
 * run while the PC can send: RTS is raised only from the end of a
 * request until its response's delimiter, and the PC, on RTS/CTS flow
 * control, holds its output meanwhile.  Everything else - decoding,
 * filesystem calls, drawing - happens with RTS dropped. */

#define KC_IDLE_TICKS      SERIAL_TICKS_PER_SECOND /* a second without a byte */
#define KC_RX_LIMIT        (2u * (KC_MAX_ENCODED + 1u)) /* bytes per answer */
#define KC_RETRIES         3u
#define KC_DRAIN_LIMIT     16u

static int clock_handle;
static unsigned char sequence;
static unsigned char decoded[KC_MAX_DECODED];
static unsigned char tx_encoded[KC_MAX_ENCODED + 1u];
static unsigned char rx_encoded[KC_MAX_ENCODED];

/* Drop what a late answer left in the receive FIFO (RTS is down, so the
 * PC is not adding to it). */
static void serial_drain(void)
{
	unsigned char left = KC_DRAIN_LIMIT;

	while (left-- != 0u && serial_getc_wait(0u) >= 0) {
	}
}

int kc_link_open(void)
{
	int result = serial_hold();

	if (result < 0) {
		return result == -KOS_EINVAL ? KC_LINK_ETYPE : KC_LINK_EDEVICE;
	}
	serial_rts(0u);
	clock_handle = timer_open();
	if (clock_handle < 0) {
		(void)serial_release();
		clock_handle = 0;
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
	/* The driver restores the channel, RTS asserted, and the console's
	 * serial rescue path works again. */
	(void)serial_release();
}

int kc_link_time(uint32_t *now)
{
	return timer_now(clock_handle, now);
}

/* Receive with RTS raised; return with it dropped.  Each COBS record is
 * read byte by byte up to its zero delimiter; RTS drops before it is
 * decoded and rises again only when the record was not the answer. */
static int receive_response(const struct kc_frame *request,
			    struct kc_frame *response)
{
	unsigned int used = 0u;
	unsigned int left = KC_RX_LIMIT;

	for (;;) {
		unsigned int raw_length;
		int byte = serial_getc_wait(KC_IDLE_TICKS);

		if (byte < 0 || left-- == 0u) {
			serial_rts(0u);
			return KC_LINK_ETIMEOUT;
		}
		if (byte != 0) {
			if (used < KC_MAX_ENCODED) {
				rx_encoded[used++] = (unsigned char)byte;
			} else {
				used = 0u;
			}
			continue;
		}
		if (used == 0u) {
			continue;
		}
		serial_rts(0u);
		raw_length = kc_cobs_decode(rx_encoded, used, decoded,
					    sizeof(decoded));
		used = 0u;
		if (raw_length != 0u &&
		    kc_frame_unpack(decoded, raw_length, response) ==
		    KC_FRAME_OK &&
		    response->kind == KC_KIND_RESPONSE &&
		    response->command == request->command &&
		    response->sequence == request->sequence &&
		    response->flags == 0u) {
			return 0;
		}
		serial_rts(1u);
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
	for (attempt = 0u; attempt < KC_RETRIES; ++attempt) {
		serial_drain();
		if (serial_write(tx_encoded, wire_length) != (int)wire_length) {
			result = KC_LINK_EWRITE;
			continue;
		}
		/* The request is out (its last byte still shifting): the PC
		 * may answer from here on. */
		serial_rts(1u);
		result = receive_response(request, response);
		if (result == 0) {
			return 0;
		}
	}
	return result;
}
