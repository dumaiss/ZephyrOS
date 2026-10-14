/* Zephyr-80 IRQ/vector mapping constants (C visible).
 *
 * IM2 vector-page geometry, the common IRQ transition state/stack
 * reservations, and the kernel-owned I-register value.  Keep this file
 * value-identical with irq.inc; the host parity test
 * (tests/test_source_layout.py) enforces it.
 *
 * Hardware port numbers and CTC/SIO register constants stay in
 * hardware.h/.inc.  General memory-region geometry stays in memory.h/.inc.
 */

#ifndef ZEPHYR80_IRQ_H
#define ZEPHYR80_IRQ_H

/* IM2 vector page: 256-byte-aligned FD00h page plus the vector-FFh guard
 * byte at FE00h (the FFh vector reads one byte past the nominal page). */
#define KOS_IRQ_VECTOR_BASE         0xFD00u
#define KOS_IRQ_VECTOR_SIZE         0x0101u

/* Common IRQ transition state: a fixed header followed by the saved
 * interrupted state, starting at FE01h. */
#define KOS_IRQ_STATE_BASE          0xFE01u
#define KOS_IRQ_STATE_HEADER_SIZE   0x0006u

/* 128-byte common IRQ transition stack, directly above the state block so
 * that an overrun of the reservation can only reach the state cells.  The
 * region above its top belongs to the common boot stack (memory.h): a
 * nucleus chain may hold live frames below FF00h, and the gate must save
 * into the reservation, not into those frames. */
#define KOS_IRQ_STACK_BASE          0xFE08u
#define KOS_IRQ_STACK_TOP           0xFE88u
#define KOS_IRQ_STACK_SIZE          0x0080u

/* Kernel-owned IM2 vector page base (the value loaded into the I register). */
#define KOS_IM2_I_VALUE             0xFDu

#endif /* ZEPHYR80_IRQ_H */
