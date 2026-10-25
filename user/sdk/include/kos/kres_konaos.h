/* KRES: the KonaOS edition's install pieces (libkres).
 *
 * The platform glue installs one of the two io tables and the XMEM
 * catalog cache, once, before the first container call:
 *
 * - readers: kres_install(&kres_io_konaos_read, &kres_cache_xmem,
 *   &ctx) — no mutation member is linked;
 * - writers: kres_install_mut(&kres_io_konaos, &kres_cache_xmem,
 *   &ctx) — the full adapter plus the mutation machinery.
 *
 * The catalog cache is one 2 KiB XMEM object per open container
 * (compaction holds two), allocated at open and freed at close.
 */

#ifndef KOS_KRES_KONAOS_H
#define KOS_KRES_KONAOS_H

#include "kos/kres.h"

/* The read side: open/close/size/read/seek/sync; the mutation-side
 * slots are NULL. */
extern const struct kres_io kres_io_konaos_read;

/* The full adapter, over the SDK file API.  A write beyond the
 * physical end extends the file with zero records; `truncate` shrinks
 * only; the compaction swap names map to 8.3. */
extern const struct kres_io kres_io_konaos;

/* The catalog cache in XMEM: a 2 KiB object per open container. */
extern const struct kres_cache kres_cache_xmem;

#endif /* KOS_KRES_KONAOS_H */
