/* ZephyrOS IPC acceptance test contract (test artifact).
 *
 * Shared by the six synthetic bank-6 drivers (tests/target/ipc), the
 * driver images' boot manifests (tests/target/ipc/manifests) and the
 * client program (ipc.c).  It is the package-39 conformance protocol,
 * never the native ABI: the frame layout, syscall numbers and descriptor
 * conventions come from abi/zos/ipc.h and drivers/include/driver.h, and
 * every constant here belongs to the synthetic driver set.
 *
 * The synthetic set is the spike's (work package 35): `echo` (the null
 * call), `con` (console-like), the `vfs -> fat -> ioc` chain (512-byte
 * chained reads, direct and staged) and `irqx` (the IRQ-owning driver).
 * Since package 40 each one is a real driver image: a hot segment
 * (_CODE), a boot segment (_BOOT/_BOOT_DATA) and its DRIVER_IO data
 * (_DRIVER_IO), packed by tools/zmodpack.py, listed in the boot manifest
 * and loaded by the nucleus driver loader.
 */

#ifndef ZEPHYROS_TARGET_IPC_TEST_H
#define ZEPHYROS_TARGET_IPC_TEST_H

#include "zos/ipc.h"
#include "zos/types.h"

/* --- endpoint classes --------------------------------------------------- */
/* Test-only class range: 1-6 are the production driver classes
 * (abi/zos/protocol.h), and the IPC image boots the real SIO and console
 * drivers beside this synthetic set. */

#define IPC_CLASS_ECHO 16u
#define IPC_CLASS_CON  17u
#define IPC_CLASS_VFS  18u
#define IPC_CLASS_FAT  19u
#define IPC_CLASS_IOC  20u
#define IPC_CLASS_IRQX 21u

/* Every synthetic endpoint exports version 1, instance 0. */
#define IPC_ENDPOINT_VERSION 1u

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
 * IN, IRQX_COPY_BYTES long) into the driver's own I/O buffer with the
 * timer armed on a short time constant, so a delivery lands inside a
 * path-B aperture window.  The reply is payload[0..1] = 16-bit sum of the
 * driver buffer, payload[2] = deliveries, payload[3] = copies; returns 0
 * or a negated errno.  Clears the delivery counter, so the client's
 * absolute-count checks start from zero afterwards. */
#define IRQX_COPY_BYTES 512u
#define IPC_IRQX_TC_APERTURE 0x08u /* short CTC0 period, inside a copy   */

/* --- frame flags -------------------------------------------------------- */

#define IPC_FRAME_STAGED 0x01u /* stage the block through the provider    */
#define IPC_FRAME_SLOW_IO 0x02u /* driver I/O buffer outside 8000h-BFFFh */
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

/* Driver images are placed by the real loader (kernel/driver/
 * driver_loader.c) from the boot manifest: the hot segment in bank 6's
 * hot tier (6000h-DFFFh), the io segment in 8000h-BFFFh (the DRIVER_IO
 * rule of 36.8), the boot segment in 0000h-5FFFh.  The loader reports each
 * placement on the early console; the replay pins the reported addresses
 * against the plan the packer prints. */
#define IPC_DRIVER_IO_BASE 0x8000u
#define IPC_DRIVER_IO_END 0xC000u

/* The `con` driver's deliberate out-of-range copy target: a buffer in its
 * own hot segment, which is outside 8000h-BFFFh, so the copy takes path C
 * and is counted as slow (correction 01 to architecture 36.8). */
#define IPC_CON_SLOW_BYTES 256u

/* The boot-segment run marker every synthetic driver records in its own
 * boot-segment BSS: the replay reads it back to prove exactly one
 * execution. */
#define IPC_BOOT_MAGIC 0xA7u

/* The IRQ source the `irqx` driver owns (enum irq_source_t: IRQ_CTC0). */
#define IPC_IRQ_CTC0 0u

/* --- small helpers ------------------------------------------------------ */

#define IPC_WORD_LO(value) ((uint8_t)((uint16_t)(value) & 0xFFu))
#define IPC_WORD_HI(value) ((uint8_t)((uint16_t)(value) >> 8))

#define IPC_FRAME_WORD(bytes, offset) \
	((uint16_t)((bytes)[offset] | \
		    ((uint16_t)(bytes)[(offset) + 1u] << 8)))

#endif /* ZEPHYROS_TARGET_IPC_TEST_H */
