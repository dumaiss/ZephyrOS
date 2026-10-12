/* KonaOS platform interface.
 *
 * This header is the single platform entry point kernel code needs: public
 * fixed-width types plus the Zephyr-80 constant headers that belong under
 * platform/zephyr80/ (hardware ports/registers, memory regions, IRQ/vector
 * wiring).  Kernel code must not hard-code port numbers or board constants;
 * it includes this header instead.
 */

#ifndef KONAOS_PLATFORM_H
#define KONAOS_PLATFORM_H

#include "kos/types.h"
#include "hardware.h"
#include "memory.h"
#include "irq.h"

/* The raw memory-latch and bank primitives are kernel-private M1 memory
 * mechanisms and live in kernel/memory/banking.h.  This header stays a
 * declarative constant aggregator; hardware-owning drivers that need a
 * board constant include the narrow header under platform/zephyr80/
 * directly. */

#endif /* KONAOS_PLATFORM_H */
