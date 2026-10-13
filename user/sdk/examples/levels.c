/* KonaOS far-data SDK example (work package 49d): the data segment.
 *
 * Compiled through the exe_program `DATA1=` option, this module carries
 * the two kinds of object a far-data segment holds:
 *
 *   - `const` objects go to the area `_DATA1` with `--constseg DATA1`,
 *     which the link bases at 8000h, so a static table of any size costs
 *     the program's own image nothing;
 *   - objects without an initializer go to `_DATA1B` with `--dataseg
 *     DATA1B`, the area the linker concatenates right after the image,
 *     and the loader zeroes it - they are ordinary variables that happen
 *     to live in the far bank.
 *
 * An initialized writable object is neither: SDCC would put its initial
 * value in `_INITIALIZED`, which EXE v2 has no startup support for, and
 * the packer refuses it by name.  Initialize writable data at run time.
 */

#define LEVEL_COUNT 4096u
#define LEVEL_STRIDE 512u

/* 4 KiB of table, mostly zero with one marker every 512 bytes: written as
 * C99 designated initializers, so the source stays a page and the linked
 * image is the full 4 KiB the aperture holds. */
const unsigned char levels[LEVEL_COUNT] = {
	[0] = 0x10u,
	[LEVEL_STRIDE] = 0x20u,
	[2u * LEVEL_STRIDE] = 0x30u,
	[3u * LEVEL_STRIDE] = 0x40u,
	[4u * LEVEL_STRIDE] = 0x50u,
	[5u * LEVEL_STRIDE] = 0x60u,
	[6u * LEVEL_STRIDE] = 0x70u,
	[7u * LEVEL_STRIDE] = 0x80u,
};

/* The marker at one level, read through the open aperture. */
unsigned char level_marker(unsigned int index)
{
	if (index >= (LEVEL_COUNT / LEVEL_STRIDE)) {
		return 0u;
	}
	return levels[index * LEVEL_STRIDE];
}

/* 2 KiB of zeroed variables: the loader zeroes the BSS of the segment
 * after copying the image, so this array reads as zeroes before the
 * program writes it. */
unsigned char history[2048];
