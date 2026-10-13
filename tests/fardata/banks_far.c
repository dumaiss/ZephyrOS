/* KonaOS far-data replay fixtures (work package 49d): the bank-exhaustion
 * container's far source.
 *
 * Compiled through the `FAR1=` option, so its code is the `_FAR1` area,
 * linked at 8000h.  The hand-written manifest (banks.segments.json)
 * declares six FAR_CODE rows over that one area: each row costs an
 * auxiliary bank, and six is more than a child can have.  The function is
 * never called.
 */

int banks_far_entry(int value) __banked
{
	return value;
}
