#ifndef KONAEDIT_TYPES_H
#define KONAEDIT_TYPES_H

#if defined(KE_HOST_TEST)
#include <stdint.h>
#else
#include "kos/types.h"
#endif

typedef uint32_t ke_pos_t;

#define KE_DOCUMENT_CAPACITY 65536ul
#define KE_PAGE_SIZE         1024u
#define KE_PAGE_COUNT        64u
#define KE_OBJECT_SIZE       8192u
#define KE_OBJECT_COUNT      8u
#define KE_PAGES_PER_OBJECT  8u

#endif
