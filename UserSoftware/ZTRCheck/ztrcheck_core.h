/* ztrcheck_core.h - the ZTR structural checks (docs/ZTR-Format.md).
 *
 * libsnd's open trusts the stamped content CRC; the full structural
 * validation lives here, reader-based and portable between the Z80
 * ztrcheck and its host twin.  ztrcheck_run fills a report: the
 * diagnostic enum and the file offset of the first defect, the CRC
 * state, and the header metadata for the OK summary.
 */

#ifndef ZTRCHECK_CORE_H
#define ZTRCHECK_CORE_H

#include "kos/reader.h"

/* The diagnostic enum: which check found the defect. */
enum ztrcheck_check {
	ZTRCHECK_OK = 0,
	ZTRCHECK_HEADER,	/* a fixed header field is out of format */
	ZTRCHECK_LAYOUT,	/* sections not the contiguous ordered layout */
	ZTRCHECK_PADDING,	/* bytes past the logical size, not a 1Ah tail */
	ZTRCHECK_STRINGS,	/* a pool reference does not terminate */
	ZTRCHECK_INSTRUMENT,	/* an instrument directory entry */
	ZTRCHECK_MACRO,		/* a macro block */
	ZTRCHECK_PATTERN,	/* a pattern directory entry's placement */
	ZTRCHECK_STREAM,	/* an event stream */
	ZTRCHECK_NOTES,		/* a note divisor */
	ZTRCHECK_CRC,		/* stored != computed */
	ZTRCHECK_IO		/* the reader failed mid-file */
};

#define ZTRCHECK_TEXT 48u	/* bytes kept of each pool string */

struct ztrcheck_report {
	uint8_t check;		/* enum ztrcheck_check */
	uint16_t offset;	/* file offset of the defect (0 when OK) */
	uint16_t crc_stored;	/* the 16h field; 0: unprotected */
	uint16_t crc_computed;	/* valid when crc_stored != 0 */
	uint16_t rate;
	uint8_t speed;
	uint16_t rows;
	uint16_t orders;
	uint16_t patterns;
	uint8_t instruments;
	uint16_t macro_bytes;
	uint32_t size;		/* the logical size */
	char title[ZTRCHECK_TEXT];
	char author[ZTRCHECK_TEXT];
	char source[ZTRCHECK_TEXT];
};

/* scratch: a work buffer, at least 8 bytes (larger reads faster).
 * Returns 0 when report.check is ZTRCHECK_OK, -1 otherwise. */
int ztrcheck_run(const kos_reader_t *reader, uint8_t *scratch,
		 uint16_t scratch_size, struct ztrcheck_report *report);

const char *ztrcheck_name(uint8_t check);
const char *ztrcheck_detail(uint8_t check);

#endif /* ZTRCHECK_CORE_H */
