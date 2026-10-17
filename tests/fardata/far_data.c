/* KonaOS far-data outcome acceptance.
 *
 * The production store's `/system/far_data.exe` runner: an ordinary SDK
 * program that drives the far-data surface and prints one line per
 * outcome.  It is built with `DATA1=far_data_levels.c` (a `const` table
 * in `_DATA1` plus a zeroed array in `_DATA1B`, both linked at 8000h)
 * and `FAR1=far_data_far.c`, and with `BLOBS=far_data_blob.bin`, so its
 * container carries three segment rows: one FAR_CODE and two DATA, the
 * second a mappable DATA segment and the third a plain blob.
 *
 * The checks:
 *
 *   * `farmap(1, &prev)` opens the aperture onto the DATA1 segment: the
 *     returned address is the window, the table reads through the plain
 *     C symbols, the zeroed array reads as zeroes (the loader's BSS
 *     zeroing) and a write through the window reads back;
 *   * the mapping is a mapping, not a mode: `*prev` is the caller's own
 *     latch, and `farunmap(prev)` restores it exactly;
 *   * `seginfo` describes the segment as a mappable DATA object of the
 *     linked size, which is how a program finds its far data;
 *   * a mappable object is filled by `read` straight into the open
 *     window (the buffer-identity rule, 4.9): the bytes land in the
 *     object, the blob payload beside it and the DATA1 segment keep
 *     their contents (the caller's own window memory is untouched);
 *   * `read` from `/system` into the same mapped object returns the
 *     store file's bytes;
 *   * `blob(BLOB_N)` is the blob row's handle and `xread` returns the payload
 *     file's bytes;
 *   * `xload` of a store file, mappable and plain, matches the file, and
 *     a file over the limit is `-KOS_ENOSPC` with nothing left behind;
 *   * a container whose segments need more auxiliary banks than the
 *     machine has is refused with `-KOS_ENOSPC`, and nothing leaks;
 *   * the one-window rule: a `__banked` call receives table values by
 *     copy, never a pointer into the mapping, and the trampoline
 *     restores the caller's latch around it.
 *
 * The last line is the verdict; the replay asserts the labels below.
 */

#include "kos.h"

#define MAPPABLE KOS_XMEM_FLAG_MAPPABLE

/* The container's row ids (the build's row order: FAR1, then DATA1, then
 * the blob) and the `farmap`/`blob` ordinals (1-based over the rows of
 * their kind). */
#define FAR1_ROW      1u
#define DATA1_ROW     2u
#define DATA1_N       1u       /* farmap(DATA1_N) is the DATA1 row        */
#define BLOB_N        1u       /* blob(BLOB_N) is the blob row            */
#define DATA1_SIZE    320u     /* far_table (64) + far_bss (256)          */
#define BLOB_SIZE     64u
#define KNOWN_SIZE    64u
#define BLK           512u
#define BIG_BLOCKS    96u      /* 48 KiB: over both xload limits        */
#define FULL_BLOCKS   32u      /* 16 KiB: the mappable limit itself     */

#define KNOWN_PATH  "/system/known.bin"
#define BANKS_PATH  "/system/banks.exe"
#define CARD_PATH   "/fs/far_data.tmp"
#define BIG_PATH    "/fs/big.tmp"
#define FULL_PATH   "/fs/full.tmp"

/* The banking latch (port 00h): the mapping checks read it directly. */
__sfr __at 0x00 latch_port;

/* The DATA1 segment's symbols (far_data_levels.c): 8000h and up, reached
 * through the aperture only while segment 1 is mapped. */
extern const unsigned char far_table[64];
extern unsigned char far_bss[256];

/* The FAR1 entry (far_data_far.c), `__banked`, so a plain C call goes
 * through the SDK trampoline. */
extern int far_data_far_sum(int a, int b) __banked;

/* The blob payload, byte for byte (tests/fardata/far_data_blob.bin). */
static const char blob_expected[65] =
	"far-data blob payload 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ! xy\n";
/* The store file `/system/known.bin`, byte for byte. */
static const char known_expected[65] =
	"KonaOS /system/known.bin: 64 bytes read into a mapped object..!\n";

/* far_table's 64 bytes summed, so one number covers all of them. */
#define TABLE_SUM 6540u

static unsigned char work[BLK];

static unsigned int checks;
static unsigned int failures;

static void check(const char *label, int ok)
{
	++checks;
	if (!ok) {
		++failures;
		printf("far-data: FAIL %s\r\n", label);
		return;
	}
	printf("far-data: %s OK\r\n", label);
}

static int same_bytes(const char *a, const char *b, unsigned int count)
{
	unsigned int i;

	for (i = 0u; i < count; ++i) {
		if (a[i] != b[i]) {
			return 0;
		}
	}
	return 1;
}

int main(void)
{
	struct kos_seg_info info;
	unsigned char before;
	unsigned char prev = 0u;
	unsigned char saved = 0u;
	unsigned char object_saved = 0u;
	void *window;
	unsigned int sum;
	unsigned int address = 0u;
	unsigned int i;
	int far_token;
	int handle;
	int blob_handle;
	int token;
	int status;

	console_puts("far-data: begin\r\n");

	/* --- the DATA1 segment through the aperture -------------------- */

	before = latch_port;
	window = farmap(DATA1_N, &prev);
	check("farmap-window", window == (void *)0x8000u);
	check("farmap-prev", prev == before);

	sum = 0u;
	for (i = 0u; i < sizeof(far_table); ++i) {
		sum += far_table[i];
	}
	check("table-read", sum == TABLE_SUM);

	check("bss-zeroed", far_bss[0] == 0u && far_bss[255] == 0u);
	for (i = 0u; i < sizeof(far_bss); ++i) {
		far_bss[i] = (unsigned char)(i ^ 0x5Au);
	}
	for (i = 0u; i < sizeof(far_bss); ++i) {
		if (far_bss[i] != (unsigned char)(i ^ 0x5Au)) {
			break;
		}
	}
	check("bss-write-read", i == sizeof(far_bss));

	/* The segment describes itself as mappable DATA, linked size
	 * 64 + 256. */
	check("seginfo-flags",
	      seginfo(DATA1_ROW, &info) == 0 &&
	      (info.flags & KOS_XMEM_FLAG_MAPPABLE) != 0u);
	check("seginfo-size", info.size == DATA1_SIZE);

	farunmap(prev);
	check("farunmap-latch", latch_port == before);

	/* --- read into a mappable object -------------------------------- */

	handle = xalloc(1024u, MAPPABLE);
	check("xalloc-mappable", handle >= 1);
	token = handle >= 1 ? xmap((unsigned int)handle, &address) : -1;
	check("xmap-window", token >= 1 &&
	      (address == 0x8000u || address == 0xA000u));

	if (token >= 1) {
		object_saved = aperture_open((unsigned char)token);
	}

	/* The card file, written first so the read compares against bytes
	 * this program chose. */
	status = -1;
	{
		int card = open(CARD_PATH, (unsigned char)(KFS_O_WRITE |
							   KFS_O_CREATE |
							   KFS_O_TRUNC));
		if (card >= 0) {
			status = write((unsigned int)card, known_expected,
				       KNOWN_SIZE);
			(void)close((unsigned int)card);
		}
	}
	check("card-write", status == (int)KNOWN_SIZE);

	status = -1;
	if (token >= 1) {
		int card = open(CARD_PATH, KFS_O_READ);

		if (card >= 0) {
			/* Straight into the open window (4.9): the
			 * buffer names the mapped object. */
			status = read((unsigned int)card, (void *)address,
				      KNOWN_SIZE);
			(void)close((unsigned int)card);
		}
	}
	check("read-into-window", status == (int)KNOWN_SIZE);
	check("read-window-bytes",
	      token >= 1 &&
	      same_bytes((const char *)address, known_expected, KNOWN_SIZE));

	/* `/system` into the same mapped object. */
	status = -1;
	if (token >= 1) {
		int file = open(KNOWN_PATH, KFS_O_READ);

		if (file >= 0) {
			status = read((unsigned int)file, (void *)address,
				      KNOWN_SIZE);
			(void)close((unsigned int)file);
		}
	}
	check("read-system", status == (int)KNOWN_SIZE);
	check("read-system-bytes",
	      token >= 1 &&
	      same_bytes((const char *)address, known_expected, KNOWN_SIZE));

	/* The caller's own window memory: the blob row and the DATA1
	 * segment still hold what they held. */
	blob_handle = blob(BLOB_N);
	check("blob-handle", blob_handle >= 1);
	status = blob_handle >= 1
			 ? xread((unsigned int)blob_handle, 0u, work, BLOB_SIZE)
			 : -1;
	check("blob-read", status == (int)BLOB_SIZE);
	check("blob-bytes", same_bytes((const char *)work, blob_expected,
				       BLOB_SIZE));

	/* The DATA1 segment again: its table and the pattern written through
	 * the window are untouched by the reads above. */
	window = farmap(DATA1_N, &prev);
	check("segment-untouched",
	      window == (void *)0x8000u && far_table[0] == 0xF0u &&
	      far_bss[0] == (unsigned char)(0u ^ 0x5Au) && far_bss[255] ==
	      (unsigned char)(255u ^ 0x5Au));
	if (window == (void *)0x8000u) {
		farunmap(prev);
	}

	if (token >= 1) {
		aperture_close(object_saved);
	}

	/* --- xload ------------------------------------------------------ */

	handle = xload(KNOWN_PATH, 0u);
	check("xload-plain", handle >= 1);
	status = handle >= 1 ? xread((unsigned int)handle, 0u, work, KNOWN_SIZE)
			     : -1;
	check("xload-plain-read", status == (int)KNOWN_SIZE);
	check("xload-plain-bytes",
	      same_bytes((const char *)work, known_expected, KNOWN_SIZE));
	status = handle >= 1 ? xfree((unsigned int)handle) : -1;
	check("xload-plain-free", status == 0);

	handle = xload(KNOWN_PATH, MAPPABLE);
	check("xload-mappable", handle >= 1);
	address = 0u;
	token = handle >= 1 ? xmap((unsigned int)handle, &address) : -1;
	if (token >= 1) {
		prev = aperture_open((unsigned char)token);
	}
	check("xload-mappable-bytes",
	      token >= 1 &&
	      same_bytes((const char *)address, known_expected, KNOWN_SIZE));
	if (token >= 1) {
		aperture_close(prev);
	}
	status = handle >= 1 ? xfree((unsigned int)handle) : -1;
	check("xload-mappable-free", status == 0);

	/* A file at the mappable limit, 16 KiB from the card: more than one
	 * read's descriptor, so the load takes several reads into the window.
	 * Each block is its index XOR the byte's offset. */
	status = -1;
	{
		int full = open(FULL_PATH, (unsigned char)(KFS_O_WRITE |
							   KFS_O_CREATE |
							   KFS_O_TRUNC));

		if (full >= 0) {
			status = 0;
			for (i = 0u; i < FULL_BLOCKS; ++i) {
				unsigned int k;

				for (k = 0u; k < BLK; ++k) {
					work[k] = (unsigned char)(k ^ i);
				}
				if (write((unsigned int)full, work, BLK) !=
				    (int)BLK) {
					status = -1;
					break;
				}
			}
			(void)close((unsigned int)full);
		}
	}
	handle = status == 0 ? xload(FULL_PATH, MAPPABLE) : -1;
	check("xload-mappable-full", handle >= 1);
	address = 0u;
	token = handle >= 1 ? xmap((unsigned int)handle, &address) : -1;
	status = -1;
	if (token >= 1) {
		const unsigned char *at;

		prev = aperture_open((unsigned char)token);
		at = (const unsigned char *)address;
		status = 0;
		for (i = 0u; i < FULL_BLOCKS && status == 0; ++i) {
			unsigned int k;

			for (k = 0u; k < BLK; ++k) {
				if (*at++ != (unsigned char)(k ^ i)) {
					status = -1;
					break;
				}
			}
		}
		aperture_close(prev);
	}
	check("xload-mappable-full-bytes", status == 0);
	status = handle >= 1 ? xfree((unsigned int)handle) : -1;
	check("xload-mappable-full-free", status == 0);
	(void)unlink(FULL_PATH);

	/* A file over the limit: 48 KiB is more than an object may hold,
	 * mappable or not.  The write streams it to the card. */
	for (i = 0u; i < BLK; ++i) {
		work[i] = (unsigned char)i;
	}
	status = -1;
	{
		int big = open(BIG_PATH, (unsigned char)(KFS_O_WRITE |
							 KFS_O_CREATE |
							 KFS_O_TRUNC));

		if (big >= 0) {
			status = 0;
			for (i = 0u; i < BIG_BLOCKS; ++i) {
				if (write((unsigned int)big, work, BLK) !=
				    (int)BLK) {
					status = -1;
					break;
				}
			}
			(void)close((unsigned int)big);
		}
	}
	check("big-write", status == 0);
	check("xload-over-mappable",
	      xload(BIG_PATH, MAPPABLE) == -KOS_ENOSPC);
	check("xload-over-plain", xload(BIG_PATH, 0u) == -KOS_ENOSPC);
	/* Nothing left behind: the pool still holds a 4 KiB object. */
	handle = xalloc(4096u, 0u);
	check("xload-no-leak", handle >= 1);
	status = handle >= 1 ? xfree((unsigned int)handle) : -1;
	check("xload-no-leak-free", status == 0);

	/* --- bank exhaustion -------------------------------------------- */

	status = run(BANKS_PATH, "");
	check("exec-banks-refused", status == -KOS_ENOSPC);
	/* The refusal allocated nothing: a whole bank is still available. */
	handle = xalloc(8192u, MAPPABLE);
	check("exec-no-leak", handle >= 1);
	status = handle >= 1 ? xfree((unsigned int)handle) : -1;
	check("exec-no-leak-free", status == 0);

	/* --- the one-window rule ---------------------------------------- */

	/* The far row's token, then the caller's own latch: the handle of
	 * the window is handed from the far code to the data segment and
	 * back, and the `__banked` call is made with values copied out of
	 * the table, never a pointer into the mapping. */
	far_token = segmap(FAR1_ROW);
	check("far-segmap", far_token >= 1);
	before = latch_port;
	window = farmap(DATA1_N, &saved);
	check("farmap-again", window == (void *)0x8000u);
	if (window != (void *)0x8000u) {
		console_puts("far-data: FAIL refarmap\r\n");
		++checks;
		++failures;
	} else {
		int a = far_table[0];
		int b = far_table[1];

		farunmap(saved);
		check("farunmap-again", latch_port == before);
		status = far_data_far_sum(a, b);
		check("banked-copy",
		      status == (int)(((unsigned int)far_token << 8) |
				      (unsigned int)((a + b) & 0xFF)));
		check("banked-latch", latch_port == before);
	}

	if (failures != 0u) {
		printf("far-data: %u checks, %u failures\r\n", checks,
		       failures);
		console_puts("far-data: FAIL\r\n");
		return 1;
	}
	printf("far-data: %u checks, 0 failures\r\n", checks);
	console_puts("far-data: PASS\r\n");
	return 0;
}
