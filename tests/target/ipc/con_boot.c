/* ZephyrOS IPC acceptance: the `con` driver's boot segment (test
 * artifact).
 *
 * Boot-tier code runs once through the mode-10 gate with Q = 6
 * (architecture 36.6, kernel/driver/driver_boot.asm), where bank 7 is
 * fully unmapped: it may touch its own bank, the common region and I/O
 * ports, and it must not call the bootstrap interface.  This one records
 * its single run in its own boot-segment BSS, which the simulator replay
 * reads back to prove the loader executed it exactly once.
 */

#include "ipc_test.h"

void ipc_con_boot(void);

uint16_t ipc_con_boot_runs;
uint8_t ipc_con_boot_magic;

void ipc_con_boot(void)
{
	ipc_con_boot_runs = (uint16_t)(ipc_con_boot_runs + 1u);
	ipc_con_boot_magic = IPC_BOOT_MAGIC;
}
