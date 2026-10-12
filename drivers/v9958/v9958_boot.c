/* ZephyrOS V9958 driver boot segment (work package 41).
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 36.6): the
 * whole resident bank is mapped - including this image's bulk segment,
 * the 2048-byte font - the fixed core is not, and the driver bootstrap
 * interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment performs the initial display bring-up: it hands the
 * shared hot bring-up chain the font's direct bank-6 address (a
 * boot-tier pointer into the bulk segment, which only this mapping can
 * use) and records the boot-once marker the acceptance replay reads
 * back.  The run-time display reset re-runs the same chain with the font
 * fetched through the nucleus bulk copy instead.
 */

#include "zos/protocol.h"

#include "v9958_driver.h"

/* The font image, defined by the bulk segment (drivers/v9958/v9958_font.asm). */
extern const uint8_t v9958_font_image[];

volatile uint8_t v9958_boot_runs;

void v9958_boot_entry(void)
{
	v9958_set_font_base((uint16_t)v9958_font_image);
	v9958_boot_runs = 1u;
	(void)v9958_display_bringup();
}
