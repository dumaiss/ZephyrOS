/* Private MCU command-lane protocol constants (drivers/ioc).
 *
 * This is NOT part of the public device API (include/zephyros/ioc.h).
 * It pins the deployed Zephyr-80 IOC firmware vocabulary that the
 * ZephyrOS IOC driver and its protocol-aware clients speak: MCU command
 * and response types, the PING payload layout, and the pinned firmware
 * level of the deployment.  The firmware is never modified; the
 * constants document what it advertises.
 *
 * Ordinary clients use `struct zioc_frame` / `struct zioc_ops` and never
 * include this header.  Protocol-aware clients (the HID driver, storage,
 * and the M8 target diagnostics) include it to name command types and
 * decode payload offsets as native-frame DATA offsets.
 */

#ifndef ZEPHYROS_DRIVERS_IOC_PROTO_H
#define ZEPHYROS_DRIVERS_IOC_PROTO_H

/* MCU command classes (Z80 -> MCU) and their replies.  The deployed
 * firmware answers `type | 80h`; RSP_UNKNOWN_COMMAND is the explicit
 * unknown-class reply and the only response outside that rule. */
#define IOC_CMD_PING          0x01u
#define IOC_RSP_PING          0x81u
#define IOC_RSP_UNKNOWN_CMD   0xFEu

/* PING request/reply DATA layout (DATA offset = compatibility-mailbox
 * offset minus the four mailbox header bytes).  The request carries the
 * echo pattern; the reply echoes those bytes and appends diagnostics. */
#define IOC_PING_ECHO_BYTES   16u /* echoed request bytes             */
#define IOC_PING_DATA_ECHO    0u
#define IOC_PING_DATA_LEVEL   16u /* controller firmware level        */
#define IOC_PING_DATA_POWER   17u /* power/link handshake snapshot    */
#define IOC_PING_DATA_LEN     26u /* reply declares the full payload  */

/* IOC_PING_DATA_POWER bit: the MCU reports the command lane as
 * established.  The pinned deployment drives /SYNC low for the session
 * once synchronised. */
#define IOC_PING_LINK_SYNCED  0x40u

/* Pinned deployment: controller firmware level 74 (4Ah).  Reported by
 * PING; recorded and validated by the M8 target diagnostic.  A driver
 * never *requires* this value, it reports what the controller returns. */
#define IOC_FW_LEVEL_PINNED   74u

#endif /* ZEPHYROS_DRIVERS_IOC_PROTO_H */
