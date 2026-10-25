/* KRES KonaOS edition: private glue between the adapter members (not
 * installed). */

#ifndef KRES_KONA_INTERNAL_H
#define KRES_KONA_INTERNAL_H

/* The read-side adapter functions (the mutation-side table
 * references them). */
int kres_kona_open(void *ctx, const char *path, unsigned int flags);
int kres_kona_close(void *ctx);
int kres_kona_size(void *ctx, uint32_t *out);
int kres_kona_read(void *ctx, void *dst, unsigned int size);
int kres_kona_seek(void *ctx, uint32_t offset);
int kres_kona_sync(void *ctx);

/* The SDK file handle a container ctx currently holds, or a negated
 * error. */
int kres_kona_handle(void *ctx);

/* The 8.3 spelling of a compaction swap name into `out`. */
void kres_kona_map_swap_name(char *out, const char *path);

#endif /* KRES_KONA_INTERNAL_H */
