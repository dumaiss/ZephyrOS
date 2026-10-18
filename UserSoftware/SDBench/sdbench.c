/* SDBench for KonaOS.
 *
 * Normal mode measures the cost visible to a KonaOS program: one SDK read()
 * call for each 128..4096-byte logical request.  /S instead drives the
 * controller's read-only /SHARED file protocol through KonaOS's published
 * /dev/ioc object, preserving the original benchmark's genuine 128, 256 and
 * 512-byte wire transactions.  /P adds the controller's physical-SD profile
 * page around each /S pass.  Neither mode touches hardware ports directly.
 *
 * CP/M BDOS and the destructive raw-card mode remain intentionally absent.
 */

#include "kos.h"
#include "kos/timer.h"

#define MAX_FILE_BYTES       0x00400000ul
#define MAX_REQUEST_BYTES    4096u
#define CLOCK_SAMPLES        64u
#define HISTOGRAM_BUCKETS    8u
#define REQUEST_ROWS         6u
#define SHARED_ROWS          3u

/* KonaOS /dev/ioc operations.  The application still reaches these only
 * through bind()/call()/unbind(); framing, sequencing, CRC and lane recovery
 * remain owned by the resident IOC driver. */
#define KIOC_OP_COMMAND       0u
#define KIOC_OP_REPLY         1u
#define KIOC_OP_BULK_READ     2u
#define KIOC_FRAME_SIZE       29u

/* Deployed controller command vocabulary used by the original SDBench. */
#define IOC_CMD_PROFILE       0x0Bu
#define IOC_RSP_PROFILE       0x8Bu
#define IOC_CMD_FS_OPEN       0x22u
#define IOC_RSP_FS_OPEN       0xA2u
#define IOC_CMD_FS_READ       0x23u
#define IOC_RSP_FS_READ       0xA3u
#define IOC_CMD_FS_CLOSE      0x25u
#define IOC_RSP_FS_CLOSE      0xA5u
#define IOC_RSP_UNKNOWN       0xFEu
#define IOC_PROFILE_RESET     0x01u
#define IOC_PROFILE_PAGE_SD   0x02u
#define IOC_PROFILE_LENGTH    16u
#define IOC_READY_LENGTH      8u

struct digest {
	unsigned int first;
	unsigned int second;
};

struct pass_stats {
	unsigned int request_size;
	uint32_t requests;
	uint32_t bytes;
	uint32_t elapsed;
	uint32_t minimum;
	uint32_t maximum;
	uint32_t histogram[HISTOGRAM_BUCKETS];
	struct digest digest;
};

struct profile_stats {
	uint32_t calls;
	uint32_t ticks;
	unsigned int misses;
	unsigned int retries;
	unsigned int reinits;
	unsigned char flags;
	unsigned char saturated;
};

/* TIMER counts at the original benchmark's 1, 2, 5, 10, 20, 50 and 100 ms
 * histogram edges.  One count is 25.6 us; the edges are nearest integers. */
static const unsigned int histogram_edges[HISTOGRAM_BUCKETS - 1u] = {
	39u, 78u, 195u, 391u, 781u, 1953u, 3906u
};

static const unsigned int request_sizes[REQUEST_ROWS] = {
	128u, 256u, 512u, 1024u, 2048u, 4096u
};

static unsigned char io_buffer[MAX_REQUEST_BYTES];
static struct pass_stats rows[REQUEST_ROWS];
static int clock_handle;
static uint32_t clock_overhead;
static unsigned char timing_enabled;
static unsigned char shared_mode;
static unsigned char profile_enabled;
static unsigned char ioc_handle;
static unsigned char shared_handle;
static uint32_t shared_size;
static unsigned int profile_base_misses;
static unsigned int profile_base_retries;
static unsigned int profile_base_reinits;

static void usage(void)
{
	console_puts("SDBench for KonaOS 0.2.0\r\n"
		     "Usage: sdbench [-n] PATH\r\n"
		     "       sdbench NAME /s [/p] [-n]\r\n"
		     "  PATH  KonaOS file; logical reads of 128..4096 bytes\r\n"
		     "  /s    read 8.3 NAME from controller /SHARED in 128..512-byte transactions\r\n"
		     "  /p    with /s, report physical SD profile counters\r\n"
		     "  -n    correctness pass only; do not use the KonaOS timer\r\n");
}

static unsigned char is_no_timer(const char *arg)
{
	return (unsigned char)(strcmp(arg, "-n") == 0 ||
			       strcmp(arg, "-N") == 0 ||
			       strcmp(arg, "/n") == 0 ||
			       strcmp(arg, "/N") == 0 ||
			       strcmp(arg, "--no-timer") == 0);
}

static unsigned char is_help(const char *arg)
{
	return (unsigned char)(strcmp(arg, "-h") == 0 ||
			       strcmp(arg, "--help") == 0 ||
			       strcmp(arg, "/?") == 0);
}

static unsigned char is_shared(const char *arg)
{
	return (unsigned char)(strcmp(arg, "-s") == 0 ||
			       strcmp(arg, "-S") == 0 ||
			       strcmp(arg, "/s") == 0 ||
			       strcmp(arg, "/S") == 0 ||
			       strcmp(arg, "--shared") == 0);
}

static unsigned char is_profile(const char *arg)
{
	return (unsigned char)(strcmp(arg, "-p") == 0 ||
			       strcmp(arg, "-P") == 0 ||
			       strcmp(arg, "/p") == 0 ||
			       strcmp(arg, "/P") == 0 ||
			       strcmp(arg, "--profile") == 0);
}

static unsigned int min_u16(unsigned int left, uint32_t right)
{
	return right < (uint32_t)left ? (unsigned int)right : left;
}

static unsigned char digest_equal(const struct digest *left,
				  const struct digest *right)
{
	return (unsigned char)(left->first == right->first &&
			       left->second == right->second);
}

/* A two-word Fletcher-style checksum.  It is computed after each timed read,
 * so validation never inflates the reported I/O latency. */
static void digest_add(struct digest *value, const unsigned char *data,
		       unsigned int length)
{
	unsigned int i;

	for (i = 0u; i < length; ++i) {
		value->first = (unsigned int)(value->first + data[i]);
		value->second = (unsigned int)(value->second + value->first);
	}
}

static uint32_t counts_to_ms100(uint32_t counts)
{
	/* timer_us() is exact for all intervals a pass in this program permits.
	 * Divide microseconds by ten for hundredths of a millisecond. */
	return (timer_us(counts) + 5u) / 10u;
}

/* The standalone SDK intentionally carries no generic 32-bit multiply
 * helper.  The report only needs small constant factors, so use the same
 * shift-and-add shape as the SDK's own freestanding arithmetic. */
static uint32_t multiply_small(uint32_t value, unsigned int factor)
{
	uint32_t result = 0u;

	while (factor != 0u) {
		if ((factor & 1u) != 0u) {
			result += value;
		}
		factor >>= 1;
		if (factor != 0u) {
			value <<= 1;
		}
	}
	return result;
}

static uint32_t get_u32(const unsigned char *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static unsigned int get_u16(const unsigned char *data)
{
	return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
}

static void put_u32(unsigned char *data, uint32_t value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
	data[2] = (unsigned char)(value >> 16);
	data[3] = (unsigned char)(value >> 24);
}

static void put_u16(unsigned char *data, unsigned int value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
}

static void ipc_request_reset(struct kos_ipc_request *request)
{
	memset(request, 0, sizeof(*request));
}

/* Submit one controller command through the resident IOC driver, then copy
 * its stored native reply into this process through the descriptor form. */
static int ioc_command(unsigned char command, const unsigned char *data,
		       unsigned char length, unsigned char expected,
		       unsigned char *reply_data, unsigned char reply_length)
{
	struct kos_ipc_request request;
	unsigned char native[KIOC_FRAME_SIZE];
	int result;

	if (length > KOS_IPC_PAYLOAD_SIZE) {
		return -KOS_EINVAL;
	}
	ipc_request_reset(&request);
	request.frame.op = KIOC_OP_COMMAND;
	request.frame.arg0 = command;
	request.frame.arg1 = length;
	if (length != 0u) {
		memcpy(request.frame.payload, data, length);
	}
	result = call(ioc_handle, &request);
	if (result < 0) {
		return result;
	}
	if (request.frame.arg0 == IOC_RSP_UNKNOWN) {
		return -KOS_ENOSYS;
	}
	if (request.frame.arg0 != expected ||
	    request.frame.arg1 != reply_length ||
	    request.frame.payload[2] != 0u) {
		return -KOS_EIO;
	}

	ipc_request_reset(&request);
	request.frame.op = KIOC_OP_REPLY;
	request.frame.arg0 = 0u;
	request.frame.arg1 = KIOC_FRAME_SIZE;
	request.buffer = native;
	request.length = KIOC_FRAME_SIZE;
	request.direction = KOS_IPC_DESC_OUT;
	result = call(ioc_handle, &request);
	if (result != KIOC_FRAME_SIZE || native[0] != expected ||
	    native[1] != 0u || native[2] != reply_length) {
		return result < 0 ? result : -KOS_EIO;
	}
	if (reply_length != 0u && reply_data != (unsigned char *)0) {
		memcpy(reply_data, &native[3], reply_length);
	}
	return 0;
}

static unsigned char name_char(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z') {
		ch = (unsigned char)(ch - ('a' - 'A'));
	}
	if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
	    ch == '$' || ch == '%' || ch == '\'' || ch == '-' || ch == '_' ||
	    ch == '@' || ch == '~') {
		return ch;
	}
	return 0u;
}

/* The legacy /SHARED command carries a packed upper-case FAT 8.3 name. */
static int pack_shared_name(const char *name, unsigned char packed[11])
{
	unsigned int base = 0u;
	unsigned int ext = 0u;
	unsigned char in_ext = 0u;

	memset(packed, ' ', 11u);
	while (*name != '\0') {
		unsigned char ch = (unsigned char)*name++;
		unsigned char converted;

		if (ch == '.') {
			if (in_ext != 0u || base == 0u) { return -KOS_EINVAL; }
			in_ext = 1u;
			continue;
		}
		converted = name_char(ch);
		if (converted == 0u) { return -KOS_EINVAL; }
		if (in_ext == 0u) {
			if (base >= 8u) { return -KOS_EINVAL; }
			packed[base++] = converted;
		} else {
			if (ext >= 3u) { return -KOS_EINVAL; }
			packed[8u + ext++] = converted;
		}
	}
	return base == 0u ? -KOS_EINVAL : 0;
}

static int ioc_open(void)
{
	unsigned char type = 0u;
	int result = bind("/dev/ioc", 0u, &type);

	if (result < 0) { return result; }
	if (type != KOS_TYPE_IOC) {
		(void)unbind((unsigned char)result);
		return -KOS_ENODEV;
	}
	ioc_handle = (unsigned char)result;
	return 0;
}

static int shared_open(const char *name)
{
	unsigned char request[12];
	unsigned char reply[5];
	int result;

	request[0] = 0u;
	result = pack_shared_name(name, &request[1]);
	if (result < 0) { return result; }
	result = ioc_command(IOC_CMD_FS_OPEN, request, sizeof(request),
			     IOC_RSP_FS_OPEN, reply, sizeof(reply));
	if (result < 0) { return result; }
	shared_handle = reply[0];
	shared_size = get_u32(&reply[1]);
	return 0;
}

static int shared_close(void)
{
	unsigned char request[5];

	memset(request, 0, sizeof(request));
	request[0] = shared_handle;
	return ioc_command(IOC_CMD_FS_CLOSE, request, sizeof(request),
			   IOC_RSP_FS_CLOSE, (unsigned char *)0, 0u);
}

static int shared_read(uint32_t offset, unsigned int length)
{
	struct kos_ipc_request request;
	unsigned char command[7];
	unsigned char ready[IOC_READY_LENGTH];
	int result;

	command[0] = shared_handle;
	put_u32(&command[1], offset);
	put_u16(&command[5], length);
	result = ioc_command(IOC_CMD_FS_READ, command, sizeof(command),
			     IOC_RSP_FS_READ, ready, sizeof(ready));
	if (result < 0) { return result; }
	if (ready[0] == 0u || ready[1] != 0u ||
	    get_u16(&ready[2]) != length) {
		return -KOS_EIO;
	}

	ipc_request_reset(&request);
	request.frame.op = KIOC_OP_BULK_READ;
	request.frame.arg0 = 0u;
	request.frame.arg1 = length;
	request.buffer = io_buffer;
	request.length = length;
	request.direction = KOS_IPC_DESC_OUT;
	result = call(ioc_handle, &request);
	return result == (int)length ? result : (result < 0 ? result : -KOS_EIO);
}

static int profile_request(unsigned char control, unsigned char reply[16])
{
	unsigned char request[2];

	request[0] = control;
	request[1] = IOC_PROFILE_PAGE_SD;
	return ioc_command(IOC_CMD_PROFILE, request, sizeof(request),
			   IOC_RSP_PROFILE, reply, IOC_PROFILE_LENGTH);
}

static int profile_begin(void)
{
	unsigned char reply[IOC_PROFILE_LENGTH];
	int result;

	if (profile_enabled == 0u) { return 0; }
	result = profile_request(IOC_PROFILE_RESET, reply);
	if (result < 0) { return result; }
	if (reply[0] != 1u) { return -KOS_EIO; }
	profile_base_misses = get_u16(&reply[10]);
	profile_base_retries = get_u16(&reply[12]);
	profile_base_reinits = get_u16(&reply[14]);
	return 0;
}

static int profile_end(struct profile_stats *stats)
{
	unsigned char reply[IOC_PROFILE_LENGTH];
	unsigned int retries;
	unsigned int reinits;
	int result;

	if (profile_enabled == 0u) { return 0; }
	result = profile_request(0u, reply);
	if (result < 0) { return result; }
	if (reply[0] != 1u) { return -KOS_EIO; }
	stats->flags = reply[1];
	stats->calls = get_u32(&reply[2]);
	stats->ticks = get_u32(&reply[6]);
	stats->misses = (unsigned int)(get_u16(&reply[10]) -
				       profile_base_misses);
	retries = get_u16(&reply[12]);
	reinits = get_u16(&reply[14]);
	stats->retries = (unsigned int)(retries - profile_base_retries);
	stats->reinits = (unsigned int)(reinits - profile_base_reinits);
	stats->saturated = (unsigned char)(
		profile_base_retries == 0xFFFFu || retries == 0xFFFFu ||
		profile_base_reinits == 0xFFFFu || reinits == 0xFFFFu);
	return 0;
}

static void print_ms(uint32_t counts)
{
	uint32_t value = counts_to_ms100(counts);

	printf("%lu.%02lu", value / 100u, value % 100u);
}

static uint32_t pass_milliseconds(const struct pass_stats *stats)
{
	uint32_t microseconds = timer_us(stats->elapsed);

	return (microseconds + 500u) / 1000u;
}

static uint32_t bytes_per_second(const struct pass_stats *stats)
{
	uint32_t milliseconds = pass_milliseconds(stats);

	if (milliseconds == 0u) {
		return 0u;
	}
	/* MAX_FILE_BYTES exists so this product remains inside 32 bits. */
	return multiply_small(stats->bytes, 1000u) / milliseconds;
}

static uint32_t kib_per_second_x100(const struct pass_stats *stats)
{
	uint32_t milliseconds = pass_milliseconds(stats);
	uint32_t scaled;

	if (milliseconds == 0u) {
		return 0u;
	}
	/* bytes * 100 / 1024 == bytes * 25 / 256.  Divide before the
	 * final factor of 1000 to keep every intermediate in 32 bits. */
	scaled = multiply_small(stats->bytes, 25u) / 256u;
	return multiply_small(scaled, 1000u) / milliseconds;
}

static void sample_add(struct pass_stats *stats, uint32_t latency)
{
	unsigned int bucket = 0u;

	stats->elapsed += latency;
	if (latency < stats->minimum) {
		stats->minimum = latency;
	}
	if (latency > stats->maximum) {
		stats->maximum = latency;
	}
	while (bucket < HISTOGRAM_BUCKETS - 1u &&
	       latency >= (uint32_t)histogram_edges[bucket]) {
		++bucket;
	}
	++stats->histogram[bucket];
}

static int calibrate_clock(void)
{
	uint32_t before;
	uint32_t after;
	uint32_t best = 0xFFFFFFFFul;
	unsigned int i;
	int result = timer_now(clock_handle, &before);

	if (result < 0) {
		return result;
	}
	for (i = 0u; i < CLOCK_SAMPLES; ++i) {
		result = timer_now(clock_handle, &after);
		if (result < 0) {
			return result;
		}
		if ((after - before) != 0u && (after - before) < best) {
			best = after - before;
		}
		before = after;
	}
	if (best == 0xFFFFFFFFul) {
		return -KOS_ETIMEDOUT;
	}
	clock_overhead = best;
	return 0;
}

static void pass_reset(struct pass_stats *stats, unsigned int request_size)
{
	unsigned int i;

	stats->request_size = request_size;
	stats->requests = 0u;
	stats->bytes = 0u;
	stats->elapsed = 0u;
	stats->minimum = 0xFFFFFFFFul;
	stats->maximum = 0u;
	stats->digest.first = 0u;
	stats->digest.second = 0u;
	for (i = 0u; i < HISTOGRAM_BUCKETS; ++i) {
		stats->histogram[i] = 0u;
	}
}

static int run_pass(int file, uint32_t size, struct pass_stats *stats)
{
	uint32_t remaining = size;
	int result;

	result = (int)seek((unsigned int)file, 0l, KFS_SEEK_SET);
	if (result < 0) {
		return result;
	}
	while (remaining != 0u) {
		unsigned int wanted = min_u16(stats->request_size, remaining);
		uint32_t before = 0u;
		uint32_t after = 0u;

		if (timing_enabled != 0u) {
			result = timer_now(clock_handle, &before);
			if (result < 0) {
				return result;
			}
		}
		result = read((unsigned int)file, io_buffer, wanted);
		if (timing_enabled != 0u) {
			int clock_result = timer_now(clock_handle, &after);

			if (clock_result < 0) {
				return clock_result;
			}
		}
		if (result < 0) {
			return result;
		}
		if ((unsigned int)result != wanted) {
			return -KOS_EIO;
		}
		if (timing_enabled != 0u) {
			uint32_t latency = after - before;

			latency = latency > clock_overhead ?
				  latency - clock_overhead : 0u;
			sample_add(stats, latency);
		}
		digest_add(&stats->digest, io_buffer, wanted);
		++stats->requests;
		stats->bytes += wanted;
		remaining -= wanted;
	}
	if (stats->minimum == 0xFFFFFFFFul) {
		stats->minimum = 0u;
	}
	return 0;
}

static int run_shared_pass(struct pass_stats *stats,
			   struct profile_stats *profile)
{
	uint32_t offset = 0u;
	int result = profile_begin();

	if (result < 0) { return result; }
	while (offset < shared_size) {
		unsigned int wanted = min_u16(stats->request_size,
					      shared_size - offset);
		uint32_t before = 0u;
		uint32_t after = 0u;

		if (timing_enabled != 0u) {
			result = timer_now(clock_handle, &before);
			if (result < 0) { return result; }
		}
		result = shared_read(offset, wanted);
		if (timing_enabled != 0u) {
			int clock_result = timer_now(clock_handle, &after);

			if (clock_result < 0) { return clock_result; }
		}
		if (result < 0) { return result; }
		if (timing_enabled != 0u) {
			uint32_t latency = after - before;

			latency = latency > clock_overhead ?
				  latency - clock_overhead : 0u;
			sample_add(stats, latency);
		}
		digest_add(&stats->digest, io_buffer, wanted);
		++stats->requests;
		stats->bytes += wanted;
		offset += wanted;
	}
	if (stats->minimum == 0xFFFFFFFFul) { stats->minimum = 0u; }
	return profile_end(profile);
}

static void report_histogram(const struct pass_stats *stats)
{
	printf("  ms <1:%lu 1-2:%lu 2-5:%lu 5-10:%lu\r\n"
	       "     10-20:%lu 20-50:%lu 50-100:%lu >=100:%lu\r\n",
	       stats->histogram[0], stats->histogram[1],
	       stats->histogram[2], stats->histogram[3],
	       stats->histogram[4], stats->histogram[5],
	       stats->histogram[6], stats->histogram[7]);
}

static void report_pass(const struct pass_stats *stats)
{
	printf("\r\n%s, %u bytes per read\r\n",
	       shared_mode != 0u ? "IOC /SHARED bulk read" :
				    "KonaOS logical read",
	       stats->request_size);
	if (timing_enabled == 0u) {
		printf("  bytes %lu  reads %lu  timing disabled\r\n",
		       stats->bytes, stats->requests);
		return;
	}
	printf("  bytes %lu  time ", stats->bytes);
	print_ms(stats->elapsed);
	printf(" ms  %lu B/s  ", bytes_per_second(stats));
	{
		uint32_t kib100 = kib_per_second_x100(stats);

		printf("%lu.%02lu KiB/s\r\n", kib100 / 100u,
		       kib100 % 100u);
	}
	printf("  reads %lu  min ", stats->requests);
	print_ms(stats->minimum);
	console_puts("  avg ");
	print_ms(stats->requests != 0u ?
		 stats->elapsed / stats->requests : 0u);
	console_puts("  max ");
	print_ms(stats->maximum);
	console_puts(" ms\r\n");
	report_histogram(stats);
}

static void report_profile(const struct profile_stats *profile,
			   const struct pass_stats *stats)
{
	uint32_t physical_ms;
	uint32_t measured_ms;

	if (profile_enabled == 0u) { return; }
	physical_ms = (profile->ticks + 15u) / 31u;
	measured_ms = pass_milliseconds(stats);
	printf("  Physical SD read calls: %lu\r\n"
	       "  Physical SD read ticks: %lu\r\n"
	       "  Physical SD read time: %lu ms (nominal); average: %lu ticks/read\r\n"
	       "  SD cache misses: %u  read retries: %u  reinits: %u\r\n",
	       profile->calls, profile->ticks, physical_ms,
	       profile->calls != 0u ? profile->ticks / profile->calls : 0u,
	       profile->misses, profile->retries, profile->reinits);
	if (timing_enabled != 0u && measured_ms != 0u &&
	    physical_ms <= 0x028F5C28ul) {
		printf("  Physical / measured I/O time: %lu%% (nominal)\r\n",
		       multiply_small(physical_ms, 100u) / measured_ms);
	}
	if (profile->flags != 0u || profile->saturated != 0u) {
		console_puts("  WARNING: recovery/error or saturated counters; SD timing suspect.\r\n");
	}
}

static void report_table(void)
{
	unsigned int i;

	if (timing_enabled == 0u) {
		return;
	}
	console_puts("\r\nRequest    Throughput      Avg lat    Max lat\r\n"
		     "------------------------------------------------\r\n");
	for (i = 0u; i < (shared_mode != 0u ? SHARED_ROWS : REQUEST_ROWS); ++i) {
		const struct pass_stats *stats = &rows[i];
		uint32_t kib100 = kib_per_second_x100(stats);

		printf("%4u B  %lu.%02lu KiB/s  ", stats->request_size,
		       kib100 / 100u, kib100 % 100u);
		print_ms(stats->elapsed / stats->requests);
		console_puts(" ms  ");
		print_ms(stats->maximum);
		console_puts(" ms\r\n");
	}
}

int main(int argc, char **argv)
{
	const char *path = (const char *)0;
	struct kfs_stat info;
	struct digest reference;
	unsigned char have_reference = 0u;
	unsigned int row_count;
	unsigned int row;
	int file = -1;
	int result;
	int i;

	timing_enabled = 1u;
	shared_mode = 0u;
	profile_enabled = 0u;
	for (i = 0; i < argc; ++i) {
		if (is_help(argv[i]) != 0u) {
			usage();
			return 0;
		}
		if (is_no_timer(argv[i]) != 0u) {
			timing_enabled = 0u;
			continue;
		}
		if (is_shared(argv[i]) != 0u) {
			shared_mode = 1u;
			continue;
		}
		if (is_profile(argv[i]) != 0u) {
			profile_enabled = 1u;
			continue;
		}
		if (path != (const char *)0) {
			console_puts("sdbench: too many paths\r\n");
			usage();
			return 1;
		}
		path = argv[i];
	}
	if (path == (const char *)0) {
		usage();
		return 1;
	}
	if (profile_enabled != 0u && shared_mode == 0u) {
		console_puts("sdbench: /p requires /s\r\n");
		usage();
		return 1;
	}

	if (shared_mode != 0u) {
		result = ioc_open();
		if (result < 0) {
			printf("sdbench: /dev/ioc unavailable (%d)\r\n", result);
			return 2;
		}
		result = shared_open(path);
		if (result < 0) {
			printf("sdbench: /SHARED/%s open failed (%d); /s needs a bare 8.3 name\r\n",
			       path, result);
			(void)unbind(ioc_handle);
			return 2;
		}
		info.size = shared_size;
		row_count = SHARED_ROWS;
	} else {
		result = stat(path, &info);
		if (result < 0) {
			printf("sdbench: stat %s failed (%d)\r\n", path, result);
			return 2;
		}
		if (info.type != KFS_TYPE_FILE) {
			printf("sdbench: %s is not a file\r\n", path);
			return 2;
		}
		file = open(path, KFS_O_READ);
		if (file < 0) {
			printf("sdbench: open %s failed (%d)\r\n", path, file);
			return 2;
		}
		row_count = REQUEST_ROWS;
	}
	if (info.size == 0u) {
		console_puts("sdbench: file is empty; nothing to measure\r\n");
		if (shared_mode != 0u) {
			(void)shared_close();
			(void)unbind(ioc_handle);
		} else {
			(void)close((unsigned int)file);
		}
		return 2;
	}
	if (info.size > MAX_FILE_BYTES) {
		console_puts("sdbench: file is over 4 MiB\r\n");
		if (shared_mode != 0u) {
			(void)shared_close();
			(void)unbind(ioc_handle);
		} else {
			(void)close((unsigned int)file);
		}
		return 2;
	}
	if (timing_enabled != 0u) {
		clock_handle = timer_open();
		if (clock_handle < 0) {
			printf("sdbench: timer unavailable (%d)\r\n", clock_handle);
			if (shared_mode != 0u) {
				(void)shared_close();
				(void)unbind(ioc_handle);
			} else {
				(void)close((unsigned int)file);
			}
			return 2;
		}
		result = calibrate_clock();
		if (result < 0) {
			printf("sdbench: timer calibration failed (%d)\r\n", result);
			if (shared_mode != 0u) {
				(void)shared_close();
				(void)unbind(ioc_handle);
			} else {
				(void)close((unsigned int)file);
			}
			return 2;
		}
	}

	console_puts("SDBench for KonaOS 0.2.0\r\n");
	if (shared_mode != 0u) {
		printf("File: /SHARED/%s  Size: %lu bytes\r\n", path, info.size);
		console_puts("Mode: IOC FS bulk; genuine 128/256/512-byte transactions\r\n");
	} else {
		printf("File: %s  Size: %lu bytes\r\n", path, info.size);
		console_puts("Mode: KonaOS FILE API; logical 128..4096-byte reads\r\n");
	}
	if (timing_enabled != 0u) {
		printf("Timer: 25.6 us/count; calibrated read overhead %lu counts\r\n",
		       clock_overhead);
	} else {
		console_puts("Timer: disabled (-n); correctness only\r\n");
	}
	console_puts("Each row is one forward pass; checksums must agree.\r\n");

	for (row = 0u; row < row_count; ++row) {
		struct profile_stats profile;

		memset(&profile, 0, sizeof(profile));
		pass_reset(&rows[row], request_sizes[row]);
		result = shared_mode != 0u ?
			run_shared_pass(&rows[row], &profile) :
			run_pass(file, info.size, &rows[row]);
		if (result < 0) {
			if (result == -(int)KOS_ENOSYS && profile_enabled != 0u) {
				console_puts("sdbench: /p needs diagnostic/profile controller firmware\r\n");
			}
			printf("sdbench: %u-byte pass failed (%d) after %lu bytes\r\n",
			       rows[row].request_size, result, rows[row].bytes);
			if (shared_mode != 0u) {
				(void)shared_close();
				(void)unbind(ioc_handle);
			} else {
				(void)close((unsigned int)file);
			}
			return 3;
		}
		if (have_reference == 0u) {
			reference = rows[row].digest;
			have_reference = 1u;
		} else if (digest_equal(&reference, &rows[row].digest) == 0u) {
			printf("sdbench: VERIFY FAIL in %u-byte pass\r\n",
			       rows[row].request_size);
			if (shared_mode != 0u) {
				(void)shared_close();
				(void)unbind(ioc_handle);
			} else {
				(void)close((unsigned int)file);
			}
			return 4;
		}
		report_pass(&rows[row]);
		report_profile(&profile, &rows[row]);
	}

	if (shared_mode != 0u) {
		result = shared_close();
		(void)unbind(ioc_handle);
	} else {
		result = close((unsigned int)file);
	}
	if (result < 0) {
		printf("sdbench: close failed (%d)\r\n", result);
		return 3;
	}
	report_table();
	printf("\r\nChecksum: %04x:%04x  PASS\r\n",
	       reference.first, reference.second);
	return 0;
}
