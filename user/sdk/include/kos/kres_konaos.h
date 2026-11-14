/* KRES: the KonaOS edition's install pieces (libkres).
 *
 * The platform glue installs one of the two io tables once, before the
 * first container call:
 *
 * - readers: kres_install(&kres_io_konaos_read, &ctx) — no mutation
 *   member and no XMEM is linked; lookups read the sorted catalog one
 *   512-byte record at a time;
 * - writers: kres_install_mut(&kres_io_konaos, &kres_cache_xmem,
 *   &ctx) — the full adapter, the mutation machinery and the XMEM
 *   catalog store.
 *
 * The catalog store holds a writable container's (or the builder's)
 * whole catalog in 16 KiB XMEM chunks of 256 entries, allocated as the
 * catalog grows and freed at close.
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

/* The catalog store in XMEM: up to 16 chunks of 16 KiB (4,096
 * entries) per store, two stores (a compaction's source and target). */
extern const struct kres_cache kres_cache_xmem;

#endif /* KOS_KRES_KONAOS_H */
