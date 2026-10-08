/* ZephyrOS IPC acceptance test contract (test artifact).
 *
 * Shared by the six synthetic bank-6 services (tests/target/ipc), the
 * acceptance loader (ipc_loader.c) and the client program (ipc.c).  It is
 * the package-39 conformance protocol, never the native ABI: the frame
 * layout, syscall numbers and descriptor conventions come from
 * abi/zos/ipc.h and include/zephyros/ipc.h, and every constant here
 * belongs to the synthetic service set.
 *
 * The synthetic set is the spike's (work package 35): `echo` (the null
 * call), `con` (console-like), the `vfs -> fat -> ioc` chain (512-byte
 * chained reads, direct and staged) and `irqx` (the IRQ-owning service).
 */

#ifndef ZEPHYROS_TARGET_IPC_TEST_H
#define ZEPHYROS_TARGET_IPC_TEST_H

#include "zos/ipc.h"
#include "zos/types.h"

/* --- endpoint classes --------------------------------------------------- */

#define IPC_CLASS_ECHO 1u
#define IPC_CLASS_CON  2u
#define IPC_CLASS_VFS  3u
#define IPC_CLASS_FAT  4u
#define IPC_CLASS_IOC  5u
#define IPC_CLASS_IRQX 6u

/* --- protocol operations ------------------------------------------------ */

#define IPC_OP_ECHO_NULL     0u /* returns ZOS_IPC_FRAME_SIZE             */
#define IPC_OP_ECHO_EXCHANGE 1u /* payload[1] = payload[0], result + 1    */
#define IPC_OP_ECHO_SELF     2u /* calls its own endpoint (refused)       */
#define IPC_OP_ECHO_DESC     3u /* returns the frame's descriptor id      */

#define IPC_OP_CON_WRITE 0u /* arg0 = descriptor offset, arg1 = length */
#define IPC_OP_CON_POLL  1u /* returns the early-console input flag    */
#define IPC_OP_CON_GETC  2u /* returns one early-console input byte    */
#define IPC_OP_CON_PROBE 3u /* payload word 0 is forged as a desc id   */

#define IPC_OP_VFS_READ    0u /* arg0 = object, arg1 = offset; payload[0..1] = length */
#define IPC_OP_VFS_SELF    1u /* arg0 = recursion depth                  */
#define IPC_OP_VFS_ON_EXIT 2u /* returns arg0 (the exit notification)    */
#define IPC_OP_VFS_EXEC    3u /* streams an executable object (arg0)     */

#define IPC_OP_FAT_READ 0u
#define IPC_OP_IOC_READ 0u

#define IPC_OP_IRQX_SPIN    0u /* arg1 = iterations; returns the run count */
#define IPC_OP_IRQX_CLAIM   1u
#define IPC_OP_IRQX_RELEASE 2u
#define IPC_OP_IRQX_COPY    3u /* arg1 = iterations; see the note below    */

/* IRQX_COPY (correction 01): runs `arg1` descriptor copies (direction
 * IN, IRQX_COPY_BYTES long) into the service's own I/O buffer with the
 * timer armed on a short time constant, so a delivery lands inside a
 * path-B aperture window.  The reply is payload[0..1] = 16-bit sum of the
 * service buffer, payload[2] = deliveries, payload[3] = copies; returns 0
 * or a negated errno.  Clears the delivery counter, so the client's
 * absolute-count checks start from zero afterwards. */
#define IRQX_COPY_BYTES 512u
#define IPC_IRQX_TC_APERTURE 0x08u /* short CTC0 period, inside a copy   */

/* --- frame flags -------------------------------------------------------- */

#define IPC_FRAME_STAGED 0x01u /* stage the block through the provider    */
#define IPC_FRAME_SLOW_IO 0x02u /* service I/O buffer outside 8000h-BFFFh */
#define IPC_FRAME_STREAM 0x04u /* the object is an executable stream      */

/* --- block geometry ----------------------------------------------------- */

#define IPC_BLOCK_BYTES    512u  /* the synthetic block the chain moves    */
#define IPC_STREAM_BYTES   512u  /* the executable object the loader reads */
#define IPC_STREAM_MAGIC   0x5Au /* first byte of an executable object     */
#define IPC_OBJECT_HANDLE  1u    /* the object handle the client holds     */

/* The IOC block pattern: byte i is IPC_IOC_PATTERN_BASE +
 * ((IPC_OBJECT_HANDLE + i) & IPC_IOC_PATTERN_MASK). */
#define IPC_IOC_PATTERN_BASE 0x40u
#define IPC_IOC_PATTERN_MASK 0x3Fu

/* --- placement ---------------------------------------------------------- */

/* The synthetic services are placed in the mode-11 window (bank 6),
 * 16-byte aligned, by the acceptance loader. */
#define IPC_SERVICE_BASE 0x8000u
#define IPC_SERVICE_ALIGN 16u

/* Service I/O buffer rule (correction 01 to architecture 36.8): every
 * buffer a service hands to copy_in/copy_out - I/O buffers, caches,
 * staging - lives in bank 6's IPC_SERVICE_BASE..IPC_SERVICE_IO_END, which
 * is where the loader places each container's data area and the one range
 * both the mode-11 window and the mode-10 aperture show at the same
 * address.  `con`'s slow target below is the deliberate exception the
 * slow-path case needs. */
#define IPC_SERVICE_IO_END 0xC000u
#define IPC_CON_SLOW_ADDRESS 0x7000u /* outside the I/O range: path C    */

/* The IRQ source the `irqx` service owns (enum irq_source_t: IRQ_CTC0). */
#define IPC_IRQ_CTC0 0u

/* --- small helpers ------------------------------------------------------ */

#define IPC_WORD_LO(value) ((uint8_t)((uint16_t)(value) & 0xFFu))
#define IPC_WORD_HI(value) ((uint8_t)((uint16_t)(value) >> 8))

#define IPC_FRAME_WORD(bytes, offset) \
	((uint16_t)((bytes)[offset] | \
		    ((uint16_t)(bytes)[(offset) + 1u] << 8)))

#endif /* ZEPHYROS_TARGET_IPC_TEST_H */
