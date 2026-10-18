/* VGMPlayer for KonaOS.
 *
 * Streams the ZVGC v1/v2 format produced by ../VGMPlayer/tools/compile_vgm.py
 * and writes conventional SN76489 bytes to Afternoon Blend PSG0.  Filesystem,
 * timer-event and diagnostic I/O access all go through the KonaOS 0.1.0 SDK.
 *
 * The TIMER driver posts a 180 Hz base event.  A phase accumulator derives
 * the file header's 1-180 Hz playback rate, matching the CP/M player's
 * scheduler.  Events coalesce while an SDK stream refill is active; the handler
 * replays every count when the process is entered again.
 */

#include "kos.h"
#include "diag.h"
#include "kos/event.h"
#include "kos/stream.h"
#include "kos/timer.h"

#define PSG_FIRST_PORT        0xE0u
#define PSG_COUNT             4u
#define PSG_MUTE_TONE0        0x9Fu

#define TIMER_BASE_RATE       180u
#define TIMER_BASE_PERIOD     217u  /* 217 * 25.6 us = 5.5552 ms */

#define BUFFER_BYTES          6144u
#define REFILL_BYTES          512u
#define STREAM_ITEM_BYTES     256u
#define STREAM_ITEMS_PER_READ 2u
#define RING_BYTES            256u

#define BUF_FREE              0u
#define BUF_READY             1u
#define BUF_ACTIVE            2u
#define BUF_FILLING           3u

#define ZVG_HEADER_BYTES      16u
#define ZVG_VERSION_V1        1u
#define ZVG_VERSION_V2        2u
#define ZVG_END               0x00u
#define ZVG_WRITE             0x01u
#define ZVG_WAIT16            0x02u
#define ZVG_WAIT_SHORT_FIRST  0x40u
#define ZVG_WAIT_SHORT_LIMIT  0x80u
#define ZVG_WRITE_RUN_FIRST   0x80u
#define ZVG_WRITE_RUN_LIMIT   0x90u

static unsigned char buffer0[BUFFER_BYTES];
static unsigned char buffer1[BUFFER_BYTES];
static unsigned int buffer0_length;
static unsigned int buffer1_length;
static unsigned char buffer0_state;
static unsigned char buffer1_state;

static FILE *file_stream;
static int file_error;
static unsigned char file_eof;
static unsigned char fill_active;
static unsigned char fill_id;
static unsigned int fill_length;

static unsigned char active_buffer;
static unsigned int stream_position;
static uint32_t stream_remaining;
static uint32_t stream_rate;
static volatile unsigned char producer_done;

/* A 256-byte ring makes both producer and consumer indices atomic on the Z80.
 * The foreground publishes tail only after copying bytes.  The event handler
 * checks that an entire command is present before consuming any of it. */
static unsigned char command_ring[RING_BYTES];
static volatile unsigned char ring_head;
static volatile unsigned char ring_tail;

static int timer_handle;
static volatile unsigned char playing;
static volatile unsigned char aborted;
static volatile unsigned char format_error;
static volatile unsigned char bad_opcode;
static volatile uint32_t bad_offset;
static volatile unsigned char psg_error;
static volatile unsigned int wait_ticks;
static volatile unsigned int tick_phase;
static volatile unsigned char tick_rate;
static volatile unsigned char key_poll_due;
static volatile unsigned char key_poll_count;
static volatile unsigned char key_poll_reload;
static volatile unsigned int base_event_count;
static volatile unsigned int playback_tick_count;
static volatile unsigned int psg_write_count;
static volatile unsigned int underrun_count;
static volatile unsigned int missed_event_count;
static volatile uint32_t stream_consumed;

static unsigned char *buffer_data(unsigned char id)
{
	return id == 0u ? buffer0 : buffer1;
}

static unsigned int buffer_length(unsigned char id)
{
	return id == 0u ? buffer0_length : buffer1_length;
}

static unsigned char buffer_state(unsigned char id)
{
	return id == 0u ? buffer0_state : buffer1_state;
}

static void set_buffer_state(unsigned char id, unsigned char state)
{
	if (id == 0u) {
		buffer0_state = state;
	} else {
		buffer1_state = state;
	}
}

static uint32_t get_le32(const unsigned char *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static unsigned char same(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0') {
			return 1u;
		}
		++left;
		++right;
	}
	return 0u;
}

static void usage(void)
{
	console_puts("VGMPLAY 0.1 for KonaOS\r\n"
		     "Usage: vgmplay FILE.ZVG\r\n"
		     "Press any key to stop playback.\r\n");
}

static int psg_port_write(unsigned char port, unsigned char value)
{
	struct kos_diag_io_out_args request;

	request.port = port;
	request.value = value;
	return diag_io_out(&request);
}

static int psg_write(unsigned char value)
{
	return psg_port_write(PSG_FIRST_PORT, value);
}

/* The four SN76489-compatible chips have no reset input and their power-on
 * state is undefined.  Initialize the card by silencing every voice on every
 * chip.  Music data remains routed to PSG0, matching the original player. */
static int mute_all_psgs(void)
{
	unsigned char chip;
	int result = 0;

	for (chip = 0u; chip < PSG_COUNT; ++chip) {
		unsigned char value = PSG_MUTE_TONE0;

		for (;;) {
			int write_result = psg_port_write(
				(unsigned char)(PSG_FIRST_PORT + chip), value);

			if (write_result < 0) {
				result = write_result;
			}
			if (value == 0xFFu) {
				break;
			}
			value = (unsigned char)(value + 0x20u);
		}
	}
	return result;
}

static void publish_fill(void)
{
	if (fill_id == 0u) {
		buffer0_length = fill_length;
		buffer0_state = fill_length != 0u ? BUF_READY : BUF_FREE;
	} else {
		buffer1_length = fill_length;
		buffer1_state = fill_length != 0u ? BUF_READY : BUF_FREE;
	}
	fill_active = 0u;
}

static void begin_fill(unsigned char id)
{
	fill_id = id;
	fill_length = 0u;
	fill_active = 1u;
	set_buffer_state(id, BUF_FILLING);
}

/* One 512-byte SDK stream read per foreground service pass.  Two 256-byte
 * items deliberately pass through the stream's measured 512-byte buffer;
 * asking fread for one 512-byte item would use its unmeasured direct path. */
static void fill_step(void)
{
	unsigned int wanted;
	unsigned int result;
	long before;
	long after;

	if (fill_active == 0u) {
		return;
	}
	wanted = (unsigned int)(BUFFER_BYTES - fill_length);
	if (wanted > REFILL_BYTES) {
		wanted = REFILL_BYTES;
	}
	before = ftell(file_stream);
	if (before < 0L) {
		file_error = -KOS_EIO;
		file_eof = 1u;
		publish_fill();
		playing = 0u;
		return;
	}
	if (wanted == REFILL_BYTES) {
		(void)fread(buffer_data(fill_id) + fill_length,
			    STREAM_ITEM_BYTES, STREAM_ITEMS_PER_READ, file_stream);
	} else {
		(void)fread(buffer_data(fill_id) + fill_length,
			    wanted, 1u, file_stream);
	}
	after = ftell(file_stream);
	if (after < before || after - before > (long)wanted) {
		file_error = -KOS_EIO;
		file_eof = 1u;
		publish_fill();
		playing = 0u;
		return;
	}
	result = (unsigned int)(after - before);
	fill_length = (unsigned int)(fill_length + result);
	if (ferror(file_stream) != 0) {
		file_error = -KOS_EIO;
		file_eof = 1u;
		publish_fill();
		playing = 0u;
		return;
	}
	if (result == 0) {
		file_eof = 1u;
		publish_fill();
		return;
	}
	if (result < wanted || feof(file_stream) != 0) {
		file_eof = 1u;
		publish_fill();
	} else if (fill_length == BUFFER_BYTES) {
		publish_fill();
	}
}

static void fill_all(unsigned char id)
{
	begin_fill(id);
	while (fill_active != 0u) {
		fill_step();
	}
}

static void service_refill(void)
{
	if (fill_active != 0u) {
		fill_step();
		return;
	}
	if (file_eof != 0u) {
		return;
	}
	if (buffer0_state == BUF_FREE) {
		begin_fill(0u);
		fill_step();
	} else if (buffer1_state == BUF_FREE) {
		begin_fill(1u);
		fill_step();
	}
}

static int validate_header(uint32_t file_size)
{
	uint32_t declared_length;
	uint32_t declared_ticks;
	uint32_t scaled_length;

	if (buffer0_length < ZVG_HEADER_BYTES || file_size < ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	if (buffer0[0] != 'Z' || buffer0[1] != 'V' ||
	    buffer0[2] != 'G' || buffer0[3] != 'C') {
		return -KOS_EINVAL;
	}
	if (buffer0[4] != ZVG_VERSION_V1 &&
	    buffer0[4] != ZVG_VERSION_V2) {
		return -KOS_ENOTSUP;
	}
	if (buffer0[5] == 0u || buffer0[5] > TIMER_BASE_RATE ||
	    buffer0[7] != ZVG_HEADER_BYTES) {
		return -KOS_EINVAL;
	}
	declared_length = get_le32(&buffer0[8]);
	if (declared_length == 0u ||
	    declared_length > file_size - ZVG_HEADER_BYTES) {
		return -KOS_EIO;
	}
	tick_rate = buffer0[5];
	stream_remaining = declared_length;
	declared_ticks = get_le32(&buffer0[12]);
	stream_rate = 0ul;
	if (declared_ticks != 0ul) {
		unsigned char factor = tick_rate;

		/* Average encoded bytes/second from the header's stream length
		 * and duration, using shift/add so SDCC needs no long multiply. */
		scaled_length = 0ul;
		while (factor != 0u) {
			if ((factor & 1u) != 0u) {
				scaled_length += declared_length;
			}
			declared_length <<= 1;
			factor >>= 1;
		}
		stream_rate = scaled_length / declared_ticks;
		if (scaled_length % declared_ticks != 0ul) {
			++stream_rate;
		}
		if (stream_rate == 0ul) {
			stream_rate = 1ul;
		}
	}
	key_poll_reload = (unsigned char)(((unsigned int)tick_rate + 9u) / 10u);
	if (key_poll_reload == 0u) {
		key_poll_reload = 1u;
	}
	key_poll_count = key_poll_reload;
	return 0;
}

static unsigned int stream_count16(uint32_t count)
{
	return count > 65535ul ? 65535u : (unsigned int)count;
}

/* Demonstrate the SDK's measured-refill planner using the ZVGC header's
 * average encoded byte rate and the timings gathered during prefetch. */
static void report_stream_plan(void)
{
	struct kos_stream_stats stats;
	struct kos_stream_need need;
	struct kos_stream_plan plan;
	int result;

	result = fstats(file_stream, &stats);
	if (result < 0) {
		printf("SDK stream: refill statistics unavailable (%d)\r\n", result);
		return;
	}
	printf("SDK stream probe: %u refills, mean %lu, worst %lu counts\r\n",
	       stats.refills, stats.mean, stats.max);
	if (stream_rate == 0ul || stats.mean == 0ul) {
		console_puts("SDK stream plan: ZVGC duration is unavailable\r\n");
		return;
	}
	need.rate = stream_rate;
	need.period = TIMER_BASE_PERIOD;
	need.chunk = KOS_STREAM_BUF_SIZE;
	need.mean = stream_count16(stats.mean);
	need.worst = stream_count16(stats.max);
	need.safety = 2u;
	result = stream_plan(&need, &plan);
	if (result < 0) {
		printf("SDK stream plan failed (%d)\r\n", result);
		return;
	}
	printf("SDK stream plan: %lu B/s, prefetch %lu bytes, refill every %u ticks\r\n",
	       stream_rate, plan.prefetch, plan.ticks_per_refill);
	printf("SDK stream plan: %u%% utilization, %lu ms startup, %s\r\n",
	       plan.utilisation_pct, plan.startup_ms,
	       plan.feasible != 0u ? "feasible" : "not feasible");
}

static void report_stream_stats(void)
{
	struct kos_stream_stats stats;

	if (file_stream != (FILE *)0 && fstats(file_stream, &stats) == 0) {
		printf("SDK stream total: %u refills, min %lu, mean %lu, max %lu counts\r\n",
		       stats.refills, stats.min, stats.mean, stats.max);
	}
}

/* Fetch one byte from the double-buffered stream.  Zero means the next buffer
 * is not ready yet or the declared stream has been completely produced. */
static unsigned char stream_get_byte(unsigned char *value)
{
	for (;;) {
		unsigned int length;

		if (stream_remaining == 0u) {
			return 0u;
		}
		length = buffer_length(active_buffer);
		if (stream_position < length) {
			*value = buffer_data(active_buffer)[stream_position++];
			--stream_remaining;
			return 1u;
		}
		{
			unsigned char next = (unsigned char)(active_buffer ^ 1u);

			if (buffer_state(next) != BUF_READY) {
				/* The header promised more stream bytes than the file
				 * delivered.  Do not turn a premature EOF into an endless
				 * sequence of ring underruns. */
				if (file_eof != 0u && fill_active == 0u) {
					file_error = -KOS_EIO;
					playing = 0u;
				}
				return 0u;
			}
			set_buffer_state(active_buffer, BUF_FREE);
			set_buffer_state(next, BUF_ACTIVE);
			active_buffer = next;
			stream_position = 0u;
		}
	}
}

/* Copy as many raw stream bytes as fit.  tail is published once after the
 * batch; the handler therefore sees either the old batch or all new bytes. */
static void producer_fill_ring(void)
{
	unsigned char tail = ring_tail;
	unsigned char value;

	while ((unsigned char)(tail + 1u) != ring_head) {
		if (stream_get_byte(&value) == 0u) {
			break;
		}
		command_ring[tail] = value;
		tail = (unsigned char)(tail + 1u);
	}
	ring_tail = tail;
	/* Publish exhaustion only after the final batch is visible.  Otherwise an
	 * event between the last copy and the tail store could misdiagnose the
	 * still-hidden final command as a truncated stream. */
	if (stream_remaining == 0u) {
		producer_done = 1u;
	}
}

static unsigned char ring_available(unsigned char head)
{
	return (unsigned char)(ring_tail - head);
}

static unsigned char ring_at(unsigned char head, unsigned char offset)
{
	return command_ring[(unsigned char)(head + offset)];
}

static void decoder_bad(unsigned char opcode, uint32_t offset)
{
	bad_opcode = opcode;
	bad_offset = offset;
	format_error = 1u;
	playing = 0u;
}

static void decoder_underrun_or_truncated(unsigned char opcode,
					 uint32_t offset)
{
	if (producer_done != 0u) {
		decoder_bad(opcode, offset);
	} else if (underrun_count != 0xFFFFu) {
		++underrun_count;
	}
}

/* One metadata-rate tick.  Consecutive writes are emitted in the same tick;
 * a wait or end command returns control to the event trampoline. */
static void decoder_tick(void)
{
	if (wait_ticks != 0u) {
		--wait_ticks;
		if (wait_ticks != 0u) {
			return;
		}
	}
	for (;;) {
		unsigned char head = ring_head;
		unsigned char available = ring_available(head);
		unsigned char opcode;
		unsigned char needed;
		unsigned char writes = 0u;
		uint32_t offset = stream_consumed + ZVG_HEADER_BYTES;

		if (available == 0u) {
			decoder_underrun_or_truncated(0xFFu, offset);
			return;
		}
		opcode = ring_at(head, 0u);
		if (opcode == ZVG_END) {
			needed = 1u;
		} else if (opcode == ZVG_WRITE) {
			needed = 2u;
			writes = 1u;
		} else if (opcode == ZVG_WAIT16) {
			needed = 3u;
		} else if (opcode >= ZVG_WAIT_SHORT_FIRST &&
			   opcode < ZVG_WAIT_SHORT_LIMIT) {
			needed = 1u;
		} else if (opcode >= ZVG_WRITE_RUN_FIRST &&
			   opcode < ZVG_WRITE_RUN_LIMIT) {
			writes = (unsigned char)((opcode & 0x0Fu) + 1u);
			needed = (unsigned char)(writes + 1u);
		} else {
			decoder_bad(opcode, offset);
			return;
		}
		if (available < needed) {
			decoder_underrun_or_truncated(opcode, offset);
			return;
		}
		if (opcode == ZVG_END) {
			ring_head = (unsigned char)(head + 1u);
			++stream_consumed;
			playing = 0u;
			return;
		}
		if (writes != 0u) {
			unsigned char index;

			for (index = 0u; index < writes; ++index) {
				if (psg_write(ring_at(head,
						      (unsigned char)(index + 1u))) < 0) {
					psg_error = 1u;
					playing = 0u;
					return;
				}
				++psg_write_count;
			}
			ring_head = (unsigned char)(head + needed);
			stream_consumed += needed;
			continue;
		}
		if (opcode == ZVG_WAIT16) {
			unsigned int ticks =
				(unsigned int)ring_at(head, 1u) |
				((unsigned int)ring_at(head, 2u) << 8);

			if (ticks == 0u) {
				decoder_bad(opcode, offset);
				return;
			}
			wait_ticks = ticks;
		} else {
			wait_ticks = (unsigned int)((opcode & 0x3Fu) + 1u);
		}
		ring_head = (unsigned char)(head + needed);
		stream_consumed += needed;
		return;
	}
}

static void on_timer_event(unsigned int count, unsigned int missed)
{
	missed_event_count = (unsigned int)(missed_event_count + missed);
	while (count != 0u && playing != 0u) {
		--count;
		++base_event_count;
		tick_phase = (unsigned int)(tick_phase + tick_rate);
		if (tick_phase >= TIMER_BASE_RATE) {
			tick_phase = (unsigned int)(tick_phase - TIMER_BASE_RATE);
			++playback_tick_count;
			if (--key_poll_count == 0u) {
				key_poll_count = key_poll_reload;
				key_poll_due = 1u;
			}
			decoder_tick();
		}
	}
}

static int start_timer(void)
{
	int result;

	if (timer_handle <= 0) {
		return -KOS_ENODEV;
	}
	result = notify(timer_handle, on_timer_event);
	if (result < 0) {
		(void)unbind((unsigned char)timer_handle);
		timer_handle = 0;
		return result;
	}
	result = timer_event(timer_handle, TIMER_BASE_PERIOD);
	if (result < 0) {
		(void)notify(timer_handle, (kos_event_handler_t)0);
		(void)unbind((unsigned char)timer_handle);
		timer_handle = 0;
		return result;
	}
	return 0;
}

static void stop_timer(void)
{
	if (timer_handle <= 0) {
		return;
	}
	(void)timer_event(timer_handle, 0u);
	(void)notify(timer_handle, (kos_event_handler_t)0);
	(void)unbind((unsigned char)timer_handle);
	timer_handle = 0;
}

static void reset_player_state(void)
{
	buffer0_length = 0u;
	buffer1_length = 0u;
	buffer0_state = BUF_FREE;
	buffer1_state = BUF_FREE;
	file_error = 0;
	file_eof = 0u;
	fill_active = 0u;
	active_buffer = 0u;
	stream_position = ZVG_HEADER_BYTES;
	stream_rate = 0ul;
	producer_done = 0u;
	ring_head = 0u;
	ring_tail = 0u;
	playing = 0u;
	aborted = 0u;
	format_error = 0u;
	bad_opcode = 0u;
	bad_offset = 0u;
	psg_error = 0u;
	wait_ticks = 0u;
	tick_phase = 0u;
	key_poll_due = 0u;
	base_event_count = 0u;
	playback_tick_count = 0u;
	psg_write_count = 0u;
	underrun_count = 0u;
	missed_event_count = 0u;
	stream_consumed = 0u;
}

int main(int argc, char **argv)
{
	const char *path;
	struct kfs_stat info;
	int result;
	int status = 0;

	if (argc != 1 || argv == (char **)0 ||
	    same(argv[0], "-h") != 0u || same(argv[0], "--help") != 0u ||
	    same(argv[0], "/?") != 0u) {
		usage();
		return argc == 1 ? 0 : 1;
	}
	path = argv[0];
	console_puts("VGMPLAY 0.1 - streamed SN76489 player for KonaOS\r\n");
	if (mute_all_psgs() < 0) {
		console_puts("vgmplay: sound card is unavailable\r\n");
		return 5;
	}

	result = stat(path, &info);
	if (result < 0 || info.type != KFS_TYPE_FILE) {
		printf("vgmplay: cannot stat %s (%d)\r\n", path, result);
		return 2;
	}
	if (info.size < ZVG_HEADER_BYTES) {
		console_puts("vgmplay: file is too short for a ZVGC header\r\n");
		return 2;
	}
	reset_player_state();
	file_stream = fopen(path, "rb");
	if (file_stream == (FILE *)0) {
		printf("vgmplay: cannot open stream %s\r\n", path);
		return 2;
	}
	timer_handle = timer_open();
	if (timer_handle < 0) {
		result = timer_handle;
		timer_handle = 0;
		printf("vgmplay: timer unavailable for stream measurement (%d)\r\n",
		       result);
		status = 5;
		goto cleanup;
	}
	result = fmeasure(file_stream, timer_handle);
	if (result < 0) {
		printf("vgmplay: cannot measure SDK stream (%d)\r\n", result);
		status = 5;
		goto cleanup;
	}
	fill_all(0u);
	if (file_error < 0 || buffer0_state != BUF_READY) {
		printf("vgmplay: initial read failed (%d)\r\n", file_error);
		status = 3;
		goto cleanup;
	}
	result = validate_header(info.size);
	if (result < 0) {
		printf("vgmplay: unsupported or damaged ZVGC file (%d)\r\n",
		       result);
		status = 4;
		goto cleanup;
	}
	buffer0_state = BUF_ACTIVE;
	if (file_eof == 0u) {
		fill_all(1u);
		if (file_error < 0) {
			printf("vgmplay: second-buffer read failed (%d)\r\n",
			       file_error);
			status = 3;
			goto cleanup;
		}
	}
	report_stream_plan();
	producer_fill_ring();

	printf("File: %s  Size: %lu bytes  Rate: %u Hz\r\n",
	       path, info.size, (unsigned int)tick_rate);
	console_puts("Playing; press any key to stop.\r\n");
	playing = 1u;
	result = start_timer();
	if (result < 0) {
		playing = 0u;
		printf("vgmplay: timer event unavailable (%d)\r\n", result);
		status = 5;
		goto cleanup;
	}

	while (playing != 0u) {
		char key;

		producer_fill_ring();
		service_refill();
		producer_fill_ring();
		if (key_poll_due != 0u) {
			key_poll_due = 0u;
			if (console_poll() > 0) {
				(void)console_read(&key, 1u);
				aborted = 1u;
				playing = 0u;
			}
		}
	}

cleanup:
	playing = 0u;
	stop_timer();
	(void)mute_all_psgs();
	report_stream_stats();
	if (file_stream != (FILE *)0) {
		int close_result = fclose(file_stream);

		if (close_result < 0 && status == 0) {
			printf("vgmplay: close failed (%d)\r\n", close_result);
			status = 3;
		}
		file_stream = (FILE *)0;
	}

	if (aborted != 0u) {
		console_puts("Playback aborted.\r\n");
	} else if (file_error < 0) {
		printf("Playback stopped by file error %d.\r\n", file_error);
		status = 3;
	} else if (psg_error != 0u) {
		console_puts("Playback stopped by PSG output error.\r\n");
		status = 5;
	} else if (format_error != 0u) {
		printf("Malformed ZVGC stream: opcode %02x at offset %lu.\r\n",
		       (unsigned int)bad_opcode, bad_offset);
		status = 4;
	} else if (status == 0) {
		console_puts("Playback complete.\r\n");
	}
	printf("Timer events %04x  ticks %04x  PSG writes %04x\r\n",
	       base_event_count, playback_tick_count, psg_write_count);
	printf("Underruns %u  missed events %u\r\n",
	       underrun_count, missed_event_count);
	if (status == 0 && aborted == 0u &&
	    (underrun_count != 0u || missed_event_count != 0u)) {
		console_puts("Playback completed with timing underruns.\r\n");
		status = 6;
	}
	return status;
}
