/* KRES shared adapter/workspace state. Separate from header/catalog helpers:
 * a reference to this resident data must not pin those helpers' code. */
#include "kres_internal.h"

/* The one shared I/O buffer: 512 bytes, never one per resource. */
uint8_t kres_shared[KRES_RECORD_SIZE];

/* The platform adapter and catalog store, installed once by the
 * platform glue before the first call. */
const struct kres_io *kres_io;
const struct kres_cache *kres_cache;
void *kres_ctx;

/* The mutation-side entry points the read path reaches without linking
 * the mutation member: kres_open's create-on-missing fallback and
 * kres_close's flush.  kres_install_mut installs both, so a program
 * that links no mutation member keeps the read path self-contained
 * (design §8, "Library size"). */
int (*kres_create_hook)(const char *path);
int (*kres_flush_hook)(kres_container *c);

void kres_install(const struct kres_io *io, void *ctx)
{
	kres_io = io;
	kres_cache = (const struct kres_cache *)0;
	kres_ctx = ctx;
}

