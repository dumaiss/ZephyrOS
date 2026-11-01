/* KonaOS IOC client-visible driver vocabulary.
 *
 * The IOC transport surface lives in drivers/include/ioc.h: the native
 * `struct kioc_frame`, the semantic status bits and the transport error
 * codes.  This header is the *vocabulary* half of the contract: it names
 * the deployed MCU command/reply classes and the client-visible DATA
 * payload offsets that a protocol-aware client must share to use the
 * transport.  It is driver vocabulary, not another device interface.
 *
 * The wire framing, the rolling sequence, CRC handling, lane setup, the
 * READY/DONE bulk admission state, bulk recovery, the frozen bulk-arming
 * class gate, and every pinned-driver implementation detail remain
 * private to drivers/ioc/.  Ordinary clients use `struct kioc_frame`
 * and never include this header.  Protocol-aware clients -- the HID
 * driver and the target diagnostics -- include it to name command types
 * and decode payload offsets as native-frame DATA offsets.
 *
 * The numeric values pin the Zephyr-80 IOC firmware at the level below
 * (Code/MCU/IOController, include/ioc_frame.h); this header documents
 * what that firmware serves.  Payload offsets are DATA offsets
 * (compatibility-mailbox offset minus the four mailbox header bytes).
 */

#ifndef KONAOS_IOC_PROTOCOL_H
#define KONAOS_IOC_PROTOCOL_H

/* The explicit unknown-class reply (RSP_UNKNOWN_COMMAND): the one
 * response outside the `type | 80h` rule, sent for a class the
 * controller does not serve (a diagnostic-profile or older firmware). */
#define IOC_RSP_UNKNOWN_CMD   0xFEu

/* PING command and reply.  The request carries an echo pattern; the
 * reply echoes those bytes and appends diagnostics. */
#define IOC_CMD_PING          0x01u
#define IOC_RSP_PING          0x81u

/* PING request/reply DATA layout. */
#define IOC_PING_ECHO_BYTES   16u /* echoed request bytes             */
#define IOC_PING_DATA_ECHO    0u
#define IOC_PING_DATA_LEVEL   16u /* controller firmware level        */
#define IOC_PING_DATA_POWER   17u /* power/link handshake snapshot    */
#define IOC_PING_DATA_LEN     26u /* reply declares the full payload  */

/* IOC_PING_DATA_POWER bit: the MCU reports the command lane as
 * established.  The pinned deployment drives /SYNC low for the session
 * once synchronised. */
#define IOC_PING_LINK_SYNCED  0x40u

/* Pinned deployment: controller firmware level 75 (4Bh).  Reported by
 * PING; recorded and validated by the IOC target diagnostic.  A driver
 * never *requires* this value, it reports what the controller returns. */
#define IOC_FW_LEVEL_PINNED   75u

/* HID input fetch (CMD_HID_INPUT / RSP_HID_INPUT).
 *
 * The one-byte request DATA is the maximum number of translated bytes
 * wanted.  The reply DATA begins with the MCU's `queued` counter and
 * its saturating `dropped` counter, followed by up to
 * IOC_HID_INPUT_MAX_DATA terminal bytes.  The controller DEQUEUES the
 * bytes it returns, so a fetch must ask for exactly the free space of
 * its local buffer: anything that does not fit locally is lost rather
 * than left queued.  Reply DATA length is therefore
 * IOC_HID_INPUT_META_LEN plus 0..IOC_HID_INPUT_MAX_DATA bytes.  The
 * translation is the MCU's; these bytes are terminal input, not raw
 * USB reports. */
#define IOC_CMD_HID_INPUT      0x0Eu
#define IOC_RSP_HID_INPUT      0x8Eu
#define IOC_OFF_HID_INPUT_MAX  0u  /* request DATA 0: wanted byte count */
#define IOC_HID_INPUT_META_LEN 2u  /* reply DATA: queued, dropped       */
#define IOC_HID_INPUT_MAX_DATA 24u /* reply DATA: most translated bytes */

/* HID mouse read (CMD_HID_MOUSE / RSP_HID_MOUSE).
 *
 * No request DATA.  The reply DATA is IOC_HID_MOUSE_LEN bytes: the
 * flags, the buttons held now (bit 0 left, 1 right, 2 middle), the
 * buttons pressed since the last read, then dx, dy and the wheel as
 * signed 16-bit little-endian motion since the last read, each capped
 * at +-32767 (+dy is down).  The controller clears dx, dy, the wheel and
 * the pressed bits on every read (handlers.c handler_hid_mouse,
 * ioc_hid.c hid_mouse_take).  Served by the normal profile only; a
 * diagnostic-profile or older controller answers IOC_RSP_UNKNOWN_CMD. */
#define IOC_CMD_HID_MOUSE         0x12u
#define IOC_RSP_HID_MOUSE         0x92u
#define IOC_OFF_HID_MOUSE_FLAGS   0u
#define IOC_OFF_HID_MOUSE_BUTTONS 1u
#define IOC_OFF_HID_MOUSE_PRESSED 2u
#define IOC_OFF_HID_MOUSE_DX      3u
#define IOC_OFF_HID_MOUSE_DY      5u
#define IOC_OFF_HID_MOUSE_WHEEL   7u
#define IOC_HID_MOUSE_LEN         9u
/* Flags: a second HID interface is mounted, and its reports decode as
 * the mouse. */
#define IOC_HID_MOUSE_MOUNTED     0x01u
#define IOC_HID_MOUSE_DECODED     0x02u

#endif /* KONAOS_IOC_PROTOCOL_H */
