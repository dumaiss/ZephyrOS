/* KonaOS libkos: stream measurement and refill planning.
 *
 * `fmeasure`/`fstats` are thin views of the stream's refill fields
 * (stdio_internal.h); `stream_plan` is pure arithmetic.  The arithmetic
 * is integer throughout - the SDK has no floating point - and every
 * figure the plan reports derives from the medium's own units, the TIMER
 * count (25.6 us at the board's 10 MHz):
 *
 *     one count  = 25.6 us          (256 T-states)
 *     b          = B * T * 16 / 625 bytes per tick
 *     P          = safety * B * t_worst + C bytes
 *     u          = B / (C / t_mean) percent of the medium
 *
 * The 25.6 us count makes the fractions awkward: b is 16/625 of a byte
 * per tick, t_mean's rate is C * 39062 / mean bytes per second, and the
 * products in between exceed 16 bits.  SDCC turns those into calls to a
 * multiply helper that this runtime does not link (docs/sdk.md section
 * 5), so the products go through `mul16` below; the single divisions
 * `__divulong` does provide carry the fractions.
 */

#include "kos.h"

#include "stdio_internal.h"

/* value * factor by shift and add.  The SDK links no multiply helper,
 * and `value * (unsigned long)factor` would want one; 16 iterations.
 * The product wraps modulo 2^32 exactly as the multiply it replaces. */
static unsigned long mul16(unsigned long value, unsigned int factor)
{
	unsigned long result = 0ul;

	while (factor != 0u) {
		if ((factor & 1u) != 0u) {
			result += value;
		}
		value <<= 1;
		factor >>= 1;
	}
	return result;
}

int fmeasure(FILE *stream, int timer)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	if (s == (struct kos_stream *)0 || timer < 0) {
		return -KOS_EINVAL;
	}
	s->timer = timer;
	s->refill_count = 0u;
	s->refill_min = 0u;
	s->refill_max = 0u;
	s->refill_sum = 0u;
	return 0;
}

int fstats(FILE *stream, struct kos_stream_stats *stats)
{
	struct kos_stream *s = KOS_STREAM_SLOT(stream);

	if (s == (struct kos_stream *)0 ||
	    stats == (struct kos_stream_stats *)0) {
		return -KOS_EINVAL;
	}
	if (s->refill_count == 0u) {
		return -KOS_ENOENT;
	}
	stats->refills = s->refill_count;
	stats->min = s->refill_min;
	stats->max = s->refill_max;
	stats->mean = s->refill_sum / s->refill_count;
	return 0;
}

int stream_plan(const struct kos_stream_need *need,
		struct kos_stream_plan *plan)
{
	unsigned int safety;
	unsigned long scaled;
	unsigned long worst_bytes;
	unsigned long medium;

	if (need == (const struct kos_stream_need *)0 ||
	    plan == (struct kos_stream_plan *)0) {
		return -KOS_EINVAL;
	}
	if (need->rate == 0ul || need->period == 0u || need->chunk == 0u ||
	    need->mean == 0u) {
		return -KOS_EINVAL;
	}
	safety = need->safety == 0u ? 2u : need->safety;

	/* b = B * T * 16/625 bytes per tick, reported in milli-bytes so
	 * a slow consumer does not lose its fraction. */
	scaled = mul16(need->rate, need->period);
	plan->bytes_per_tick_millis = scaled / 625ul * 16ul +
				      scaled % 625ul * 16ul / 625ul;

	/* n = floor(C / b): how many ticks one refill's worth lasts. */
	if (plan->bytes_per_tick_millis == 0ul) {
		/* Below a milli-byte per tick: no refill ever comes due
		 * inside the 16-bit tick count. */
		plan->ticks_per_refill = 65535u;
	} else {
		unsigned long ticks = mul16((unsigned long)need->chunk,
					    1000u) /
				      plan->bytes_per_tick_millis;

		if (ticks == 0ul) {
			ticks = 1ul;
		}
		plan->ticks_per_refill = ticks > 65535ul ? 65535u :
							   (unsigned int)ticks;
	}

	/* The worst refill's worth of bytes, then the pre-fetch, rounded
	 * up to whole chunks. */
	worst_bytes = mul16(need->rate / 39062ul, need->worst) +
		      mul16(need->rate % 39062ul, need->worst) / 39062ul;
	plan->prefetch = mul16(worst_bytes, safety) + need->chunk;
	if (plan->prefetch % need->chunk != 0ul) {
		plan->prefetch = mul16(plan->prefetch / need->chunk + 1ul,
				       need->chunk);
	}

	/* The medium's own rate, the utilisation against it, and the
	 * delay before the first pre-fetch has arrived. */
	medium = mul16((unsigned long)need->chunk, 39062u) / need->mean;
	plan->utilisation_pct = medium == 0ul ? 100u :
				(unsigned int)(mul16(need->rate, 100u) /
					       medium);
	plan->feasible = plan->utilisation_pct < 50u ? 1u : 0u;
	plan->startup_ms = medium == 0ul ? 0ul :
			   mul16(plan->prefetch, 1000u) / medium;
	return 0;
}
