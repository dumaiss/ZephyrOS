/* KonaOS versioned driver protocols (public native ABI).
 *
 * A resident driver exports endpoints; a client reaches one with the IPC
 * envelope of abi/kos/ipc.h (a bound handle, call, the fixed 32-byte
 * frame and one optional buffer descriptor per call).  This header is
 * layer 3 of the ABI (architecture 6.1): the operation numbers, the frame
 * word and payload layouts and the descriptor conventions of the
 * device-facing drivers:
 *
 *   console   class 6, instance 0: buffered WRITE, READ, GETC, POLL
 *   HID       class 4, instance 0: translated text input
 *   video     class 2, instance 0: the seven native VIDEO operations and
 *                                  the character-output path
 *   serial    class 1, instance 0: SIO0/B character input and output
 *   IOC       class 5, instance 0: the command/reply lane, the bulk
 *                                  lanes and the semantic status word
 *   timer     class 10, instance 0: the monotonic clock
 *
 * The FILE protocol (type KOS_TYPE_FILE) and the filesystem-namespace
 * protocol (type KOS_TYPE_FS) are the file language: the ROMFS
 * provider serves `/system`, the build's filesystem driver (IOC-FAT)
 * serves `/fs`, and both answer through the same operation numbers
 *.
 *
 * Every driver carries protocol version KOS_PROTOCOL_VERSION (the
 * endpoint's registered version).  Classes 1-5 are the device classes;
 * class 6 is the console driver, which is not a device class.
 *
 * Frame conventions (struct kos_ipc_frame, abi/kos/ipc.h):
 *
 *   op      the operation number below.
 *   flags   protocol flags; the operations here define none, and a
 *           driver refuses nonzero flags it does not define.
 *   desc    the nucleus-issued buffer descriptor id, or KOS_IPC_DESC_NONE
 *           when the call named no caller buffer.
 *   arg0    protocol word 0 (an offset, byte value, operation or length,
 *           per operation).
 *   arg1    protocol word 1 (a length, limit or count, per operation).
 *   payload up to 24 inline protocol bytes.
 *   status  filled by the nucleus with the driver's result.
 *
 * Buffer convention: WRITE-style operations take the caller's buffer as
 * a descriptor (captured at syscall entry) with `arg0` the byte offset
 * inside it and `arg1` the transfer length; READ-style operations name
 * the same fields with the buffer as the destination.  A descriptor
 * belongs to the call that captured it and may be passed down a call
 * chain (4.9); the operations that copy are bounds-checked by the
 * nucleus on every use, so a driver never names a caller address.
 *
 * Hot paths are batched by design (4.9): one WRITE moves a whole
 * transfer through the console and its output driver in one call, and
 * the IOC command exchange carries a whole request and returns the whole
 * reply in one call.
 *
 * Driver-to-driver data movement: drivers pass the *driver-domain*
 * address of a buffer they own in a reply, and the receiving driver
 * stages the bytes with the nucleus `move` primitive (4.9).  This is
 * the documented convention of the resident driver domain; application
 * buffers are never named this way.
 */

#ifndef KOS_PROTOCOL_H
#define KOS_PROTOCOL_H

#include "kos/types.h"

/* The one protocol version every driver in this header registers. */
#define KOS_PROTOCOL_VERSION 1u

/* Driver classes (endpoint lookup key; see the header comment). */
#define KOS_CLASS_SERIAL  1u
#define KOS_CLASS_VIDEO   2u
#define KOS_CLASS_BLOCK   3u /* reserved: the future block driver */
#define KOS_CLASS_HID     4u
#define KOS_CLASS_IOC     5u
#define KOS_CLASS_CONSOLE 6u
#define KOS_CLASS_FILE    7u /* the file driver: FILE objects and the
                              * filesystem-namespace provider behind /fs */
#define KOS_CLASS_TIMER   10u /* the timer: the monotonic clock (type
                               * KOS_TYPE_TIMER) */

/* The instance every device-facing driver registers (one instance). */
#define KOS_CLASS_INSTANCE 0u

/* --- common operations --------------------------------------------------
 *
 * Every endpoint answers these two numbers, above every class operation
 * number.  They carry the exclusive-handle contract of architecture 4.9:
 *
 *   KOS_OP_ACQUIRE (FEh)  the nucleus is about to bind one of the
 *                         endpoint's objects exclusively.  arg0 = the
 *                         object identity the bind named.  The driver
 *                         reports 0, or -KOS_EBUSY when another handle
 *                         already holds that object exclusively; any other
 *                         negative answer fails the bind with that error.
 *   KOS_OP_RELEASE (FFh)  a handle on one of the endpoint's objects went
 *                         away - unbound by its owner or released by a
 *                         process exit.  arg0 = the object, arg1 = 1 when
 *                         that handle was the exclusive one.  The driver
 *                         restores what the handle took over; the result
 *                         is ignored.
 *
 * Only an endpoint whose image declared MOD_EP_RELEASE (the manifest's
 * `"release": true`) is sent RELEASE, so a driver that owns no restoreable
 * hardware state pays nothing for the contract.  ACQUIRE reaches every
 * endpoint, because the exclusive answer is the driver's own.
 *
 *   KOS_OP_SUBSCRIBE (FDh)  an application subscribes to the endpoint's
 *                         events, or drops the subscription.  arg0 = the
 *                         subscription id (1..4) the nucleus assigned, or
 *                         0 when the subscription went away; the nucleus
 *                         keeps the handler, so the call names the id
 *                         only.  The driver reports 0, or a negative
 *                         error: -KOS_EBUSY when its one event source is
 *                         already subscribed, -KOS_EINVAL for an id it
 *                         never issued.  Only an endpoint whose image
 *                         declared MOD_EP_EVENTS (the manifest's
 *                         `"events": true`) is ever sent SUBSCRIBE: the
 *                         nucleus refuses SYS_NOTIFY on any other handle
 *                         with KOS_ENOTSUP before the driver sees
 *                         anything.
 */
#define KOS_OP_SUBSCRIBE 0xFDu
#define KOS_OP_ACQUIRE 0xFEu
#define KOS_OP_RELEASE 0xFFu

/* --- console driver (class KOS_CLASS_CONSOLE) ---------------------------
 *
 * The console composes the boot's input and output drivers (HID or SIO
 * input, V9958 or SIO output, chosen by the build) behind one endpoint.
 * It is the endpoint the SDK console stubs and the
 * monitor use; the early console and the panic path do not depend on it.
 *
 *   KCONSOLE_OP_WRITE (0)  desc IN: arg0 = offset, arg1 = length.
 *                          Returns the bytes written (0..length).
 *   KCONSOLE_OP_READ  (1)  desc OUT: arg0 = offset, arg1 = length.
 *                          Non-blocking drain: returns the bytes
 *                          transferred, 0 when nothing is pending.
 *   KCONSOLE_OP_GETC  (2)  returns one byte (0..255) or -KOS_EAGAIN.
 *   KCONSOLE_OP_POLL  (3)  returns 1 when input is pending, else 0.
 *   KCONSOLE_OP_PUTC  (4)  arg0 = byte: one output byte, no descriptor;
 *                          returns the count (1) or a negative error.
 *                          The single-byte form the SDK uses for
 *                          putchar and the nucleus uses to prove the
 *                          driver from a call with no caller buffer.
 *   KCONSOLE_OP_RESET (5)  no payload: reset the terminal to its start
 *                          state - the output dependency's own reset
 *                          (KVIDEO_OP_RESET) plus an empty input queue -
 *                          so the next program starts on a clean screen
 *                          with nothing typed for its predecessor left to
 *                          read.  Returns 0 or a negative error.
 */
#define KCONSOLE_OP_WRITE 0u
#define KCONSOLE_OP_READ  1u
#define KCONSOLE_OP_GETC  2u
#define KCONSOLE_OP_POLL  3u
#define KCONSOLE_OP_PUTC  4u
#define KCONSOLE_OP_RESET 5u

/* --- HID input driver (class KOS_CLASS_HID) -----------------------------
 *
 * Translated text input from the IOC-attached keyboard: the fetch-on-
 * doorbell engine and its local ring, with the SIO fallback supplied by
 * the console composition, not by this driver.
 *
 *   KHID_OP_READ_TEXT (0)  desc OUT: arg0 = offset, arg1 = maximum bytes.
 *                          Returns the bytes transferred (0 when the
 *                          ring is empty), or a negative error.
 *   KHID_OP_POLL      (1)  returns 1 when a byte is buffered or a fetch
 *                          delivered one, else 0.
 *   KHID_OP_GETC      (2)  returns one byte (0..255) or -KOS_EAGAIN.
 *   KHID_OP_READY     (3)  returns 1 when the engine's IOC dependency is
 *                          present and its link is up, else 0.  It is a
 *                          status report: the console is chosen by the
 *                          build and does not probe it.
 */
#define KHID_OP_READ_TEXT 0u
#define KHID_OP_POLL      1u
#define KHID_OP_GETC      2u
#define KHID_OP_READY     3u

/* --- video driver (class KOS_CLASS_VIDEO) -------------------------------
 *
 * The V9958 driver: the seven native VIDEO operations (KVIDEO_CTRL_WRITE
 * .. KVIDEO_RESET) and the character-output path the console composes.
 * A directly attached video chip implements "write control/register
 * bytes", "write VRAM data bytes", "write palette bytes", "write
 * indirect-register bytes", "present" and "reset"; it does not implement
 * packet types.
 *
 *   KVIDEO_OP_SEND  (0)  arg0 = the native VIDEO operation; desc IN with
 *                        arg0 = offset, arg1 = payload length for the
 *                        five streaming operations.  PRESENT and RESET
 *                        take no descriptor and length 0.  Returns 0 or
 *                        a negative error.
 *   KVIDEO_OP_WRITE (1)  desc IN: arg0 = offset, arg1 = length.  Feeds
 *                        the bytes to the terminal parser and returns
 *                        the bytes consumed.
 *   KVIDEO_OP_PUTC  (2)  arg0 = byte: one terminal byte; returns 0.
 *   KVIDEO_OP_RESET (3)  the full display re-init as its own operation,
 *                        so the console reset reaches it without the
 *                        native-operation wrapper: every driver-owned
 *                        register, selector, palette entry and VRAM asset
 *                        is rewritten from any state.  No payload;
 *                        returns 0 or a negative error.
 *
 * KVIDEO_RESET re-establishes every driver-owned state, including the
 * font and atlas upload; the driver reads the font from its own bulk
 * data through the nucleus bulk copy (the boot/bulk tier is not mapped
 * in the driver window).  It is the same chain KVIDEO_OP_RESET runs.
 */
#define KVIDEO_OP_SEND  0u
#define KVIDEO_OP_WRITE 1u
#define KVIDEO_OP_PUTC  2u
#define KVIDEO_OP_RESET 3u

/* The native VIDEO operations (the KVIDEO_OP_SEND argument; exact values,
 * not Virtual Drip packet numbers). */
#define KVIDEO_CTRL_WRITE     1u
#define KVIDEO_DATA_WRITE     2u
#define KVIDEO_DATA_BLOCK     3u
#define KVIDEO_PALETTE_WRITE  4u
#define KVIDEO_INDIRECT_WRITE 5u
#define KVIDEO_PRESENT        6u
#define KVIDEO_RESET          7u

/* --- serial driver (class KOS_CLASS_SERIAL) -----------------------------
 *
 * SIO0/B, the same channel as the early console, as a resident driver.
 * Polling-only: the SIO interrupt mode stays disabled, so this driver
 * owns no IRQ.
 *
 *   KSIO_OP_PUTC  (0)  arg0 = byte; returns 0 or -KOS_ETIMEDOUT.
 *   KSIO_OP_WRITE (1)  desc IN: arg0 = offset, arg1 = length; returns
 *                      the bytes written.
 *   KSIO_OP_POLL  (2)  returns 1 when a received byte is pending, else 0.
 *   KSIO_OP_GETC  (3)  returns one byte (0..255) or -KOS_EAGAIN.
 *   KSIO_OP_READ  (4)  desc OUT: arg0 = offset, arg1 = length; drains up
 *                      to `length` bytes and returns the count.
 */
#define KSIO_OP_PUTC  0u
#define KSIO_OP_WRITE 1u
#define KSIO_OP_POLL  2u
#define KSIO_OP_GETC  3u
#define KSIO_OP_READ  4u

/* --- IOC transport driver (class KOS_CLASS_IOC) -------------------------
 *
 * The command/reply lane and the bulk lanes of the Z80-to-MCU transport
 * (drivers/include/ioc.h owns the transport surface and its error
 * codes; drivers/include/ioc_protocol.h owns the command
 * vocabulary).  The driver owns the wire framing, sequence, CRC, lane
 * setup and the READY/DONE bulk admission; a client sees native frames.
 *
 *   KIOC_OP_COMMAND     (0)  arg0 = request type, arg1 = request DATA
 *                            length (0..24), payload[0..arg1-1] = request
 *                            DATA.  One exchange with the controller.
 *                            Reply frame:
 *                              arg0 = reply type
 *                              arg1 = reply DATA length (0..26)
 *                              payload[0..1] = the 29-byte reply frame's
 *                                              driver-domain address
 *                              payload[2] = reply STATUS
 *                              payload[3..4] = this driver's bulk buffer
 *                                              address (the staging
 *                                              handshake for a caller
 *                                              that cannot hold a
 *                                              descriptor; see below)
 *                            Returns 0 or a negative KIOC_E*.
 *                            The full request frame is 29 bytes; no
 *                            deployed request class exceeds 24 DATA
 *                            bytes, so the exchange control travels in
 *                            the frame words.  A class that ever needs
 *                            the full field adds an explicit descriptor
 *                            operation.
 *   KIOC_OP_REPLY       (1)  desc OUT: arg0 = offset into the stored reply
 *                            frame (0..29), arg1 = length.  Copies the
 *                            reply frame slice KIOC_OP_COMMAND stored to
 *                            the caller's buffer; returns the count.
 *   KIOC_OP_BULK_READ   (2)  arg1 = length (1..512).  Completes the
 *                            admitted bulk read into the driver buffer.
 *                            With a desc OUT the driver also copies the
 *                            bytes to the caller (arg0 = offset);
 *                            without one the reply carries the buffer
 *                            address in payload[0..1], for the staged
 *                            form.  Returns the length or a negative
 *                            KIOC_E*.
 *   KIOC_OP_BULK_WRITE  (3)  arg1 = length (1..512).  With a desc IN the
 *                            driver first stages the caller's bytes
 *                            (arg0 = offset) into its buffer, then writes
 *                            them to the lane; without one it writes what
 *                            the staged form left in the buffer.  Returns
 *                            the length or a negative KIOC_E*.
 *   KIOC_OP_STATUS      (4)  returns the semantic status word
 *                            (KIOC_STATUS_*).
 *   KIOC_OP_READY       (5)  returns 1 when the link is brought up and
 *                            no recovery is pending, else 0.  The
 *                            driver's init runs the bounded LINK_SYNC
 *                            bring-up once; the HID driver reports this
 *                            through KHID_OP_READY.
 *
 * The staged form (no descriptor) is the resident-domain convention:
 * the reply's payload names the driver buffer, and a driver client
 * stages with `move`.  A nucleus (bank-7) caller reaches the same
 * buffers through the nucleus copy gate because it cannot hold a
 * descriptor: it reads the
 * reply frame and the bulk payload out of the addresses the replies
 * name, places bulk-write bytes there, and then commits them with the
 * descriptor-less KIOC_OP_BULK_WRITE.
 */
#define KIOC_OP_COMMAND    0u
#define KIOC_OP_REPLY      1u
#define KIOC_OP_BULK_READ  2u
#define KIOC_OP_BULK_WRITE 3u
#define KIOC_OP_STATUS     4u
#define KIOC_OP_READY      5u

/* The native frame geometry the IOC protocol moves (drivers/include/
 * ioc.h): TYPE, STATUS, LENGTH, DATA[26]. */
#define KIOC_PROTOCOL_FRAME_SIZE 29u
#define KIOC_PROTOCOL_FRAME_DATA 26u
/* The largest request DATA one KIOC_OP_COMMAND carries inline (the
 * frame's 24-byte payload). */
#define KIOC_PROTOCOL_REQUEST_MAX 24u

/* --- timer driver (class KOS_CLASS_TIMER, type KOS_TYPE_TIMER) ----------
 *
 * The monotonic clock: CTC channel 2 runs as a 156.25 Hz tick (250
 * counts of 25.6 µs) and the driver's handler counts the ticks.  A bound
 * handle is the clock; the first two operations are stateless.  All three
 * are descriptor-less: a clock reading is four bytes of protocol data,
 * never a caller buffer.
 *
 *   KTIMER_OP_NOW  (0)  the reply payload holds the current count in
 *                       payload[0..3], little-endian.  The count wraps
 *                       after about 30.5 hours; elapsed values use
 *                       unsigned subtraction.
 *   KTIMER_OP_INFO (1)  the reply payload holds the clock constants:
 *                       payload[0..3] = counts per 10 s and
 *                       payload[4..7] = counts per tick, both
 *                       little-endian.
 *   KTIMER_OP_EVENT (2) arm the driver's event timer (CTC channel 3) or
 *                       stop it: payload[0..3] = the period in counts,
 *                       little-endian, 0 = stop.  One subscription at a
 *                       time: a second subscribe, or an arm while the
 *                       timer runs, is -KOS_EBUSY; arming without a
 *                       subscription is -KOS_EINVAL.  The timer posts the
 *                       event at absolute deadlines (the period is exact
 *                       from count to count, whatever the count rate's
 *                       granularity), so a subscriber sees no drift.  The
 *                       channel stays off until an arm, and the handle's
 *                       release stops it; see docs/devices.md.
 *
 * The two constants below are the clock's fixed values: the count rate is
 * the 10 MHz system clock through the 256 prescaler (39,062.5 counts per
 * second), the tick rate is 156.25 Hz.
 */
#define KTIMER_OP_NOW   0u
#define KTIMER_OP_INFO  1u
#define KTIMER_OP_EVENT 2u

#define KOS_TIMER_COUNTS_PER_10S  390625u
#define KOS_TIMER_COUNTS_PER_TICK 250u

/* --- FILE protocol (type KOS_TYPE_FILE) --------------------------------
 *
 * The filesystem object protocol (architecture 4.10 and 6.2, the file
 * language): `bind(path, flags)` yields a FILE handle and the operations
 * below act on the bound instance - the ROMFS provider in process for
 * `/system`, the selected filesystem driver for `/fs`.
 *
 * Instance identity: `payload[0]` of every operation names the bound
 * instance (the provider's object identity).  The FS provider stamps it
 * from the caller's handle before it forwards the call to its driver, so
 * a bank-6 dispatch finds its per-open state without a handle table of
 * its own; the in-process ROMFS provider reads its instance from the
 * handle entry.
 *
 *   KFILE_OP_READ (0)  desc OUT: arg0 = offset inside the caller buffer,
 *                      arg1 = length.  Reads from the instance's position
 *                      and advances it.  Returns the bytes transferred
 *                      (0 at end of file).
 *   KFILE_OP_WRITE (1) desc IN: arg0 = offset, arg1 = length.  Returns
 *                      the bytes written; a read-only provider answers
 *                      KOS_EROFS.
 *   KFILE_OP_SEEK (2)  arg0 = KFS_SEEK_SET/CUR/END and payload[1..4] = a
 *                      little-endian signed 32-bit offset; the reply
 *                      carries the new position in payload[1..4] and the
 *                      status is 0.
 *   KFILE_OP_META (3)  no descriptor; the reply payload holds a packed
 *                      `struct kfs_stat` (16 bytes, abi/kos/fs.h).
 *   KFILE_OP_SYNC (4)  no descriptor; flushes the instance (the file
 *                      close path is SYNC then unbind).
 */
#define KFILE_OP_READ  0u
#define KFILE_OP_WRITE 1u
#define KFILE_OP_SEEK  2u
#define KFILE_OP_META  3u
#define KFILE_OP_SYNC  4u

/* --- filesystem-namespace protocol (type KOS_TYPE_FS) -------------------
 *
 * `bind(path, 0)` on a provider root - `bind("/fs", 0)` or
 * `bind("/system", 0)` - yields a filesystem-namespace handle: the
 * caller's key to the name operations that act on paths rather than on an
 * open file (architecture 4.10).  `mkdir`, `rmdir`, `unlink`, `rename`
 * and `stat` in `fs.h` build on it; there is no DIRECTORY handle and no
 * universal directory protocol.
 *
 * The application passes the **path remainder** - the target path with
 * the bound provider root removed, e.g. `/GAMES` for `/fs/GAMES` - in the
 * call's one descriptor.  `stat` replies the packed `struct kfs_stat` in
 * the payload; every other operation replies only its status.  The
 * operation numbers start at 08h so a dispatch shared with the FILE
 * protocol (0-4 above) tells the two apart without consulting the type.
 *
 *   KFS_OP_MKDIR  (8)  desc IN: the path to create.
 *   KFS_OP_RMDIR  (9)  desc IN: the path to remove (must be empty).
 *   KFS_OP_UNLINK (10, 0Ah)  desc IN: the file to remove.
 *   KFS_OP_RENAME (11, 0Bh)  desc IN: the source path, NUL, then the
 *                      target path.
 *   KFS_OP_STAT   (12, 0Ch)  desc IN: the path; the reply payload holds a
 *                      packed `struct kfs_stat`.
 *
 * A provider refuses a name operation it cannot serve (ROMFS answers
 * KOS_EROFS for every mutation) and owns the meaning of the path below
 * its root: the FS provider forwards the remainder to the filesystem
 * driver (kernel/vfs/fs.c), which performs it.
 *
 * **Nucleus transport.**  Between the nucleus provider and a bank-6
 * filesystem driver the same operations travel with the path staged in a
 * nucleus request buffer (the fixed core, always visible to a driver)
 *
 *   KFS_OP_OPEN  (13, 0Dh)  payload[0..1] = path address, payload[2..3] =
 *                      KFS_O_* flags; reply payload[0] = the provider
 *                      instance identity.  The FS provider's `bind`.
 *   KFS_OP_CLOSE (14, 0Eh)  payload[0] = instance; the provider's
 *                      `unbind`.
 *   KFS_OP_ENUM  (15, 0Fh)  desc OUT: the caller's entry array; payload[0..1]
 *                      = path address, payload[2..3] = cookie,
 *                      payload[4..5] = the caller's byte count.  Writes
 *                      up to that many `struct kos_ns_entry` records,
 *                      replies the next cookie in payload[2..3], returns
 *                      the entry count, and is what `ns_enum` below
 *                      `/fs` runs (architecture 4.10).
 */
#define KFS_OP_MKDIR  8u
#define KFS_OP_RMDIR  9u
#define KFS_OP_UNLINK 10u
#define KFS_OP_RENAME 11u
#define KFS_OP_STAT   12u
#define KFS_OP_OPEN   13u
#define KFS_OP_CLOSE  14u
#define KFS_OP_ENUM   15u

#endif /* KOS_PROTOCOL_H */
