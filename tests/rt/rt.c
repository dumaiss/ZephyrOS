/* KonaOS real-time interrupt outcome program (card program, run from the
 * production shell by tests/test_rt.py).
 *
 *   rt.exe main      the ABI end to end, exiting with the interrupt open
 *   rt.exe child     run by `main` while it holds the interrupt
 *   rt.exe idle      twenty idle() waits under the RT clock
 *   rt.exe read F    read file F at RT rates 0/1k/8k/16k (board diagnosis)
 *   rt.exe nosupport the open against a controller without the RT clock
 *   rt.exe xmem      objects across the NMI entry, written and read back
 *                    under the RT clock (page zero checked by the replay)
 *
 * One line per outcome; the replay asserts the lines and the machine
 * state the kernel and the IO controller are left in.
 */

#include "kos.h"
#include "kos/timer.h"

#define RT_HANDLER ((void *)KOS_RT_REGION_BASE)
#define RT_COUNT   (*(volatile uint16_t *)0xF100u)

extern const uint8_t rt_handler_image[];
extern const uint8_t rt_handler_image_end[];

static uint8_t same(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') {
			return 1u;
		}
		++a;
		++b;
	}
	return 0u;
}

static void install(void)
{
	memcpy(RT_HANDLER, rt_handler_image,
	       (unsigned int)(rt_handler_image_end - rt_handler_image));
	RT_COUNT = 0u;
}

static uint16_t ticks(void)
{
	uint16_t a;
	uint16_t b;

	do {
		a = RT_COUNT;
		b = RT_COUNT;
	} while (a != b);
	return a;
}

static int run_main(void)
{
	struct kos_rt_info info;
	int timer = timer_open();
	int result;
	uint16_t count;

	result = rt_info(&info);
	printf("rt: info %d %04x %04x %u\r\n", result, info.region_base,
	       info.region_size, (unsigned int)info.held);
	printf("rt: outside %d\r\n", rt_open((void *)0x1000u, 16000u));
	install();
	printf("rt: slow %d\r\n", rt_open(RT_HANDLER, 500u));
	printf("rt: fast %d\r\n", rt_open(RT_HANDLER, 40000u));
	printf("rt: open %d\r\n", rt_open(RT_HANDLER, 16000u));
	(void)rt_info(&info);
	printf("rt: held %u\r\n", (unsigned int)info.held);
	printf("rt: again %d\r\n", rt_open(RT_HANDLER, 16000u));
	/* 50 ms at 16 kHz: 800 ticks, give or take the reads' edges. */
	count = ticks();
	(void)timer_wait_us(timer, 50000u);
	count = (uint16_t)(ticks() - count);
	printf("rt: ticked %u\r\n", (unsigned int)count);
	printf("rt: system %d\r\n", open("/system/shell.exe", KFS_O_READ));
	printf("rt: reboot %d\r\n", reboot());
	printf("rt: child %d\r\n", run("/fs/rt.exe", "child"));
	/* Still running after the child's refusal. */
	count = ticks();
	(void)timer_wait_us(timer, 10000u);
	printf("rt: alive %u\r\n", (unsigned int)(ticks() != count));
	printf("rt: PASS\r\n");
	/* Exit with the interrupt open: the exit must release it. */
	return 0;
}

static int run_child(void)
{
	struct kos_rt_info info;

	(void)rt_info(&info);
	printf("rt: child held %u\r\n", (unsigned int)info.held);
	printf("rt: child open %d\r\n", rt_open(RT_HANDLER, 8000u));
	return 7;
}

static int run_nosupport(void)
{
	struct kos_rt_info info;

	install();
	printf("rt: nosupport %d\r\n", rt_open(RT_HANDLER, 16000u));
	(void)rt_info(&info);
	printf("rt: nosupport held %u\r\n", (unsigned int)info.held);
	return 0;
}

static uint8_t pattern(unsigned int object, unsigned int at)
{
	return (uint8_t)(at ^ (at >> 8) ^ (object * 0x35u));
}

/* Six one-slot objects take the data slots of the bank they open in
 * preference order (3, 6, then 0, 1, 2), so one lies at slot 0, across
 * the NMI entry.  Every byte is written and read back while the RT clock
 * runs, and the program exits with the objects' contents in place for
 * the replay. */
static int run_xmem(void)
{
	static uint8_t buf[256];
	unsigned int handle[6];
	unsigned int count = 0u;
	unsigned int bad = 0u;
	unsigned int i;
	unsigned int at;
	unsigned int k;
	uint16_t start;

	install();
	printf("rt: xmem open %d\r\n", rt_open(RT_HANDLER, 16000u));
	start = ticks();
	for (i = 0u; i < 6u; ++i) {
		int h = xalloc(8192u, 0u);

		if (h < 1) {
			break;
		}
		handle[count++] = (unsigned int)h;
		for (at = 0u; at < 8192u; at += sizeof(buf)) {
			for (k = 0u; k < sizeof(buf); ++k) {
				buf[k] = pattern(i, at + k);
			}
			(void)xwrite((unsigned int)h, at, buf, sizeof(buf));
		}
	}
	for (i = 0u; i < count; ++i) {
		for (at = 0u; at < 8192u; at += sizeof(buf)) {
			(void)xread(handle[i], at, buf, sizeof(buf));
			for (k = 0u; k < sizeof(buf); ++k) {
				bad += buf[k] != pattern(i, at + k);
			}
		}
	}
	printf("rt: xmem objects %u bad %u ticked %u\r\n", count, bad,
	       (unsigned int)(ticks() != start));
	return 0;
}

/* A file read at each RT rate (0 = clock off): bytes read, the first
 * error, and the ticks counted, so a lane failure under the clock shows
 * which rate brings it on. */
static int run_read(const char *path)
{
	static uint8_t buf[2048];
	static const unsigned int rates[4] = {0u, 1000u, 8000u, 16000u};
	unsigned int i;

	install();
	for (i = 0u; i < 4u; ++i) {
		uint32_t total = 0u;
		int error = 0;
		int file;
		int opened = 0;
		uint16_t start;

		if (rates[i] != 0u) {
			opened = rt_open(RT_HANDLER, rates[i]);
			if (opened != 0) {
				printf("rt: read %u open %d\r\n", rates[i], opened);
				continue;
			}
		}
		start = ticks();
		file = open(path, KFS_O_READ);
		if (file < 0) {
			error = file;
		} else {
			while (total < 262144ul) {
				int got = read((unsigned int)file, buf,
					       sizeof(buf));

				if (got <= 0) {
					error = got;
					break;
				}
				total += (uint32_t)got;
			}
			(void)close((unsigned int)file);
		}
		printf("rt: read %u bytes %lu error %d ticks %u\r\n", rates[i],
		       total, error, (unsigned int)(ticks() - start));
		if (rates[i] != 0u) {
			(void)rt_close();
		}
	}
	return 0;
}

/* idle() under the RT clock: twenty waits are twenty timer ticks, not
 * twenty NMIs. */
static int run_idle(void)
{
	int timer = timer_open();
	uint32_t t0 = 0u;
	uint32_t t1 = 0u;
	uint16_t start;
	unsigned int i;

	install();
	printf("rt: idle open %d\r\n", rt_open(RT_HANDLER, 16000u));
	(void)timer_now(timer, &t0);
	start = ticks();
	for (i = 0u; i < 20u; ++i) {
		idle();
	}
	(void)timer_now(timer, &t1);
	printf("rt: idle counts %lu nmis %u\r\n", t1 - t0,
	       (unsigned int)(ticks() - start));
	(void)rt_close();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 1 && same(argv[0], "child") != 0u) {
		return run_child();
	}
	if (argc >= 2 && same(argv[0], "read") != 0u) {
		return run_read(argv[1]);
	}
	if (argc >= 1 && same(argv[0], "idle") != 0u) {
		return run_idle();
	}
	if (argc >= 1 && same(argv[0], "nosupport") != 0u) {
		return run_nosupport();
	}
	if (argc >= 1 && same(argv[0], "xmem") != 0u) {
		return run_xmem();
	}
	return run_main();
}
