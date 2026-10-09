/* ZephyrOS versioned service protocols (public native ABI, M10 stage 4).
 *
 * A resident service exports endpoints; a client reaches one with the IPC
 * envelope of abi/zos/ipc.h (lookup, call, the fixed 32-byte frame and
 * one optional buffer descriptor per call).  This header is layer 3 of
 * the M10 ABI (architecture 36.14): the operation numbers, the frame
 * word and payload layouts and the descriptor conventions of the five
 * device-facing services that package 41 moves into bank 6:
 *
 *   console   class 6, instance 0: buffered WRITE, READ, GETC, POLL
 *   HID       class 4, instance 0: translated text input
 *   video     class 2, instance 0: the seven native VIDEO operations and
 *                                  the character-output path
 *   serial    class 1, instance 0: SIO0/B character input and output
 *   IOC       class 5, instance 0: the command/reply lane, the bulk
 *                                  lanes and the semantic status word
 *
 * Every service carries protocol version ZOS_PROTOCOL_VERSION (the
 * endpoint's registered version, returned by SYS_IPC_LOOKUP).  Classes
 * keep the numeric slots the M8 device contracts pinned (ZDEV_SERIAL,
 * ZDEV_VIDEO, ZDEV_BLOCK, ZDEV_HID, ZDEV_IOC, include/zephyros/device.h);
 * class 6 is the console service, which is not a device class.
 *
 * Frame conventions (struct zos_ipc_frame, abi/zos/ipc.h):
 *
 *   op      the operation number below.
 *   flags   protocol flags; the operations here define none, and a
 *           service refuses nonzero flags it does not define.
 *   desc    the nucleus-issued buffer descriptor id, or ZOS_IPC_DESC_NONE
 *           when the call named no caller buffer.
 *   arg0    protocol word 0 (an offset, byte value, operation or length,
 *           per operation).
 *   arg1    protocol word 1 (a length, limit or count, per operation).
 *   payload up to 24 inline protocol bytes.
 *   status  filled by the nucleus with the service's result.
 *
 * Buffer convention: WRITE-style operations take the caller's buffer as
 * a descriptor (captured at syscall entry) with `arg0` the byte offset
 * inside it and `arg1` the transfer length; READ-style operations name
 * the same fields with the buffer as the destination.  A descriptor
 * belongs to the call that captured it and may be passed down a call
 * chain (36.8); the operations that copy are bounds-checked by the
 * nucleus on every use, so a service never names a caller address.
 *
 * Hot paths are batched by design (36.7): one WRITE moves a whole
 * transfer through the console and its output service in one call, and
 * the IOC command exchange carries a whole request and returns the whole
 * reply in one call.
 *
 * Service-to-service data movement: services pass the *service-domain*
 * address of a buffer they own in a reply, and the receiving service
 * stages the bytes with the nucleus `move` primitive (36.8).  This is
 * the documented convention of the resident service domain; application
 * buffers are never named this way.
 */

#ifndef ZOS_PROTOCOL_H
#define ZOS_PROTOCOL_H

#include "zos/types.h"

/* The one protocol version every service in this header registers. */
#define ZOS_PROTOCOL_VERSION 1u

/* Service classes (endpoint lookup key; see the header comment). */
#define ZOS_CLASS_SERIAL  1u
#define ZOS_CLASS_VIDEO   2u
#define ZOS_CLASS_BLOCK   3u /* reserved: the future block service */
#define ZOS_CLASS_HID     4u
#define ZOS_CLASS_IOC     5u
#define ZOS_CLASS_CONSOLE 6u

/* The instance every device-facing service registers (one instance). */
#define ZOS_CLASS_INSTANCE 0u

/* --- console service (class ZOS_CLASS_CONSOLE) ---------------------------
 *
 * The console composes the boot's input and output services (HID or SIO
 * input, V9958 or SIO output) behind one endpoint, exactly as the M8
 * console binding did.  It is the endpoint the SDK console stubs and the
 * monitor use; the early console and the panic path do not depend on it.
 *
 *   ZCONSOLE_OP_WRITE (0)  desc IN: arg0 = offset, arg1 = length.
 *                          Returns the bytes written (0..length).
 *   ZCONSOLE_OP_READ  (1)  desc OUT: arg0 = offset, arg1 = length.
 *                          Non-blocking drain: returns the bytes
 *                          transferred, 0 when nothing is pending.
 *   ZCONSOLE_OP_GETC  (2)  returns one byte (0..255) or -ZOS_EAGAIN.
 *   ZCONSOLE_OP_POLL  (3)  returns 1 when input is pending, else 0.
 *   ZCONSOLE_OP_PUTC  (4)  arg0 = byte: one output byte, no descriptor;
 *                          returns the count (1) or a negative error.
 *                          The single-byte form the SDK uses for
 *                          zos_putc and the nucleus uses to prove the
 *                          service from a call with no caller buffer.
 */
#define ZCONSOLE_OP_WRITE 0u
#define ZCONSOLE_OP_READ  1u
#define ZCONSOLE_OP_GETC  2u
#define ZCONSOLE_OP_POLL  3u
#define ZCONSOLE_OP_PUTC  4u

/* --- HID input service (class ZOS_CLASS_HID) -----------------------------
 *
 * Translated text input from the IOC-attached keyboard: the fetch-on-
 * doorbell engine and its local ring, with the SIO fallback supplied by
 * the console composition, not by this service.
 *
 *   ZHID_OP_READ_TEXT (0)  desc OUT: arg0 = offset, arg1 = maximum bytes.
 *                          Returns the bytes transferred (0 when the
 *                          ring is empty), or a negative error.
 *   ZHID_OP_POLL      (1)  returns 1 when a byte is buffered or a fetch
 *                          delivered one, else 0.
 *   ZHID_OP_GETC      (2)  returns one byte (0..255) or -ZOS_EAGAIN.
 *   ZHID_OP_READY     (3)  returns 1 when the engine's IOC dependency is
 *                          present and its link is up, else 0.  The
 *                          console composes its input at boot from this
 *                          probe, the role the device registry's
 *                          registration presence played before M10.
 */
#define ZHID_OP_READ_TEXT 0u
#define ZHID_OP_POLL      1u
#define ZHID_OP_GETC      2u
#define ZHID_OP_READY     3u

/* --- video service (class ZOS_CLASS_VIDEO) -------------------------------
 *
 * The V9958 driver: the seven native VIDEO operations of
 * include/zephyros/video.h (ZVIDEO_CTRL_WRITE .. ZVIDEO_RESET) and the
 * character-output path the console composes.
 *
 *   ZVIDEO_OP_SEND  (0)  arg0 = the native VIDEO operation; desc IN with
 *                        arg0 = offset, arg1 = payload length for the
 *                        five streaming operations.  PRESENT and RESET
 *                        take no descriptor and length 0.  Returns 0 or
 *                        a negative error.
 *   ZVIDEO_OP_WRITE (1)  desc IN: arg0 = offset, arg1 = length.  Feeds
 *                        the bytes to the terminal parser and returns
 *                        the bytes consumed.
 *   ZVIDEO_OP_PUTC  (2)  arg0 = byte: one terminal byte; returns 0.
 *
 * ZVIDEO_RESET re-establishes every driver-owned state, including the
 * font and atlas upload; the service reads the font from its own bulk
 * data through the nucleus bulk copy (the boot/bulk tier is not mapped
 * in the service window).
 */
#define ZVIDEO_OP_SEND  0u
#define ZVIDEO_OP_WRITE 1u
#define ZVIDEO_OP_PUTC  2u

/* --- serial service (class ZOS_CLASS_SERIAL) -----------------------------
 *
 * SIO0/B, the same channel as the early console, as a resident service.
 * Polling-only: the SIO interrupt mode stays disabled, so this service
 * owns no IRQ.
 *
 *   ZSIO_OP_PUTC  (0)  arg0 = byte; returns 0 or -ZDEV_ETIMEDOUT.
 *   ZSIO_OP_WRITE (1)  desc IN: arg0 = offset, arg1 = length; returns
 *                      the bytes written.
 *   ZSIO_OP_POLL  (2)  returns 1 when a received byte is pending, else 0.
 *   ZSIO_OP_GETC  (3)  returns one byte (0..255) or -ZOS_EAGAIN.
 *   ZSIO_OP_READ  (4)  desc OUT: arg0 = offset, arg1 = length; drains up
 *                      to `length` bytes and returns the count.
 */
#define ZSIO_OP_PUTC  0u
#define ZSIO_OP_WRITE 1u
#define ZSIO_OP_POLL  2u
#define ZSIO_OP_GETC  3u
#define ZSIO_OP_READ  4u

/* --- IOC transport service (class ZOS_CLASS_IOC) -------------------------
 *
 * The command/reply lane and the bulk lanes of the Z80-to-MCU transport
 * (include/zephyros/ioc.h owns the class semantics and the transport
 * error codes; include/zephyros/ioc_protocol.h owns the command
 * vocabulary).  The service owns the wire framing, sequence, CRC, lane
 * setup and the READY/DONE bulk admission; a client sees native frames.
 *
 *   ZIOC_OP_COMMAND     (0)  arg0 = request type, arg1 = request DATA
 *                            length (0..24), payload[0..arg1-1] = request
 *                            DATA.  One exchange with the controller.
 *                            Reply frame:
 *                              arg0 = reply type
 *                              arg1 = reply DATA length (0..26)
 *                              payload[0..1] = the 29-byte reply frame's
 *                                              service-domain address
 *                              payload[2] = reply STATUS
 *                              payload[3..4] = this service's bulk buffer
 *                                              address (the staging
 *                                              handshake for a caller
 *                                              that cannot hold a
 *                                              descriptor; see below)
 *                            Returns 0 or a negative ZIOC_E*.
 *                            The full request frame is 29 bytes; no
 *                            deployed request class exceeds 24 DATA
 *                            bytes, so the exchange control travels in
 *                            the frame words.  A class that ever needs
 *                            the full field adds an explicit descriptor
 *                            operation.
 *   ZIOC_OP_REPLY       (1)  desc OUT: arg0 = offset into the stored reply
 *                            frame (0..29), arg1 = length.  Copies the
 *                            reply frame slice ZIOC_OP_COMMAND stored to
 *                            the caller's buffer; returns the count.
 *   ZIOC_OP_BULK_READ   (2)  arg1 = length (1..512).  Completes the
 *                            admitted bulk read into the service buffer.
 *                            With a desc OUT the service also copies the
 *                            bytes to the caller (arg0 = offset);
 *                            without one the reply carries the buffer
 *                            address in payload[0..1], for the staged
 *                            form.  Returns the length or a negative
 *                            ZIOC_E*.
 *   ZIOC_OP_BULK_WRITE  (3)  arg1 = length (1..512).  With a desc IN the
 *                            service first stages the caller's bytes
 *                            (arg0 = offset) into its buffer, then writes
 *                            them to the lane; without one it writes what
 *                            the staged form left in the buffer.  Returns
 *                            the length or a negative ZIOC_E*.
 *   ZIOC_OP_STATUS      (4)  returns the semantic status word
 *                            (ZIOC_STATUS_*).
 *   ZIOC_OP_READY       (5)  returns 1 when the link is brought up and
 *                            no recovery is pending, else 0.  The
 *                            service's init runs the bounded LINK_SYNC
 *                            bring-up once; the HID service probes this
 *                            to decide whether its engine is usable (and
 *                            the console then falls back to SIO), which
 *                            is the role the IOC device registration
 *                            presence played before M10.
 *
 * The staged form (no descriptor) is the resident-domain convention:
 * the reply's payload names the service buffer, and a service client
 * stages with `move`.  The module path (the arena IOC-FAT provider
 * until package 42) reaches the same buffers through the nucleus copy
 * gate because a bank-7 caller cannot hold a descriptor: it reads the
 * reply frame and the bulk payload out of the addresses the replies
 * name, places bulk-write bytes there, and then commits them with the
 * descriptor-less ZIOC_OP_BULK_WRITE.
 */
#define ZIOC_OP_COMMAND    0u
#define ZIOC_OP_REPLY      1u
#define ZIOC_OP_BULK_READ  2u
#define ZIOC_OP_BULK_WRITE 3u
#define ZIOC_OP_STATUS     4u
#define ZIOC_OP_READY      5u

/* The native frame geometry the IOC protocol moves (include/zephyros/
 * ioc.h): TYPE, STATUS, LENGTH, DATA[26]. */
#define ZIOC_PROTOCOL_FRAME_SIZE 29u
#define ZIOC_PROTOCOL_FRAME_DATA 26u
/* The largest request DATA one ZIOC_OP_COMMAND carries inline (the
 * frame's 24-byte payload). */
#define ZIOC_PROTOCOL_REQUEST_MAX 24u

#endif /* ZOS_PROTOCOL_H */
