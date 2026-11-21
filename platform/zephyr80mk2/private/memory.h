/* Zephyr-80 Mk2 memory-map constants (C visible).
 *
 * The common page (architecture 2.7.2): DAT entries 7 and 15 name the
 * same SRAM page, so logical E000h-FFFFh is the same memory in the system
 * and the working bank.  It keeps the Mk1 common layout (architecture
 * 2.4) and adds the DAT state (see memory.inc).  Keep this file
 * value-identical with memory.inc (parity-tested).
 */

#ifndef ZEPHYR80MK2_MEMORY_H
#define ZEPHYR80MK2_MEMORY_H

/* The low entry stubs every page-0 image carries (0000h-00FFh). */
#define KOS_PAGE_ZERO_END           0x0100u
#define KOS_NMI_ENTRY               0x0066u

/* The system bank's ISR stack: the top of logical page 2. */
#define KOS_IRQ_ISR_STACK_BASE      0x5F00u
#define KOS_IRQ_ISR_STACK_TOP       0x6000u
#define KOS_IRQ_ISR_STACK_SIZE      0x0100u

/* The common page and its packed-run ceiling. */
#define KOS_COMMON_BASE             0xE000u
#define KOS_COMMON_CEILING          0x0C00u
#define KOS_STACK_INIT_SP           0x0000u
#define KOS_GATE_STACK_SIZE         384u

/* The real-time interrupt's vector and its idle RETN. */
#define KOS_RT_VECTOR               0xEF08u
#define KOS_RT_IDLE                 0xEF0Bu

/* DAT state: the bank mirrors, the control mirror and the identity of
 * the image the working bank holds. */
#define KOS_DAT_STATE_BASE          0xEF10u
#define KOS_DAT_SYSTEM              0xEF10u
#define KOS_DAT_WORKING             0xEF18u
#define KOS_DAT_CONTROL             0xEF20u
#define KOS_DAT_CACHED              0xEF21u
#define KOS_DAT_STATE_SIZE          0x0013u

#endif /* ZEPHYR80MK2_MEMORY_H */
