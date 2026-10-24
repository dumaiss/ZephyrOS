#ifndef KONAEDIT_STORE_H
#define KONAEDIT_STORE_H

#include "ke_types.h"

#define KE_STORE_OK       0
#define KE_STORE_EINVAL  -1
#define KE_STORE_ENOSPC  -2
#define KE_STORE_EIO     -3

struct ke_store {
	uint16_t handles[KE_OBJECT_COUNT];
	uint8_t order[KE_PAGE_COUNT];
	uint16_t page_length[KE_PAGE_COUNT];
	uint8_t page_count;
	uint8_t cache_page;
	uint8_t cache_dirty;
	ke_pos_t length;
	uint8_t cache[KE_PAGE_SIZE];
};

/* No position: a row past the document's end. */
#define KE_POS_NONE 0xFFFFFFFFul

int ke_store_init(struct ke_store *store);
void ke_store_dispose(struct ke_store *store);
int ke_store_flush(struct ke_store *store);
ke_pos_t ke_store_length(const struct ke_store *store);
int ke_store_read(struct ke_store *store, ke_pos_t at, void *buffer,
		  uint16_t length);
int ke_store_insert(struct ke_store *store, ke_pos_t at,
		    const void *buffer, uint16_t length);
int ke_store_delete(struct ke_store *store, ke_pos_t at, ke_pos_t length);
int ke_store_replace_byte(struct ke_store *store, ke_pos_t at, uint8_t value);
int ke_store_byte(struct ke_store *store, ke_pos_t at, uint8_t *value);
/* The cached page holding `at` (< length): its bytes, `at`'s offset in it
 * and its length.  The pointer is valid until the next store call. */
int ke_store_span(struct ke_store *store, ke_pos_t at, const uint8_t **page,
		  uint16_t *offset, uint16_t *page_length);
/* The bytes equal to `value` in [from, to). */
uint32_t ke_store_count(struct ke_store *store, ke_pos_t from, ke_pos_t to,
			uint8_t value);

#endif
