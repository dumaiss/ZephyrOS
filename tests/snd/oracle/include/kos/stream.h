/* Host shim: the reference players use the stream planner only in their
 * foreground UI paths, which the oracle tools do not call.  The types and
 * declarations exist so the sources compile unmodified; the host stdio
 * provides the actual reads. */

#ifndef SND_ORACLE_KOS_STREAM_H
#define SND_ORACLE_KOS_STREAM_H

#include <stdio.h>
#include <stdint.h>

#define KOS_STREAM_BUF_SIZE 512u

struct kos_stream_stats {
	uint32_t refills;
	uint32_t min;
	uint32_t mean;
	uint32_t max;
};

struct kos_stream_need {
	uint32_t rate;
	uint32_t period;
	uint32_t chunk;
	uint32_t mean;
	uint32_t worst;
	uint32_t safety;
};

struct kos_stream_plan {
	uint32_t prefetch;
	uint32_t ticks_per_refill;
	uint32_t utilisation_pct;
	uint32_t startup_ms;
	unsigned char feasible;
};

int fmeasure(FILE *stream, int timer_handle);
int fstats(FILE *stream, struct kos_stream_stats *stats);
int stream_plan(const struct kos_stream_need *need,
		struct kos_stream_plan *plan);

#endif /* SND_ORACLE_KOS_STREAM_H */
