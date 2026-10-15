/* KonaOS kernel-private device-event interface (kernel/event).
 *
 * A device event is a signal a resident driver raises for an application
 * that subscribed to it: the driver keeps owning its interrupt and calls
 * event_post(), and the nucleus delivers the event to the subscriber - once
 * the subscriber is the process running again - by building a 24-byte frame
 * on the program's own stack and returning to the libkos trampoline.  The
 * rules are architecture 4.5/4.10; the frame layout, the pick/enter/unwind
 * primitives and every cell they read live in kernel/event/event_common.asm
 * (common memory: the delivery runs under the application's own latch).
 *
 * This header owns the table shape the assembly and the C halves share, the
 * fixed-core post path and the cold-half syscall and teardown bodies.  The
 * table is 4 subscriptions, one per (process, handle) pair, plus one
 * `in_handler` flag per process slot.
 *
 * Ledger (docs/memory.md): the table and the delivery state are permanent
 * common memory, reachable from driver-interrupt context, which runs with
 * the driver's window mapped and the bank-7 data area unmapped.
 */

#ifndef KONAOS_KERNEL_EVENT_EVENT_H
#define KONAOS_KERNEL_EVENT_EVENT_H

#include "kos/types.h"

/* Subscriptions: one row per (process, handle).  A second NOTIFY on a
 * handle the process already subscribed to replaces the handler. */
#define KOS_EVENT_MAX 4u

/* Row stride in bytes; the assembly walks the table with this stride and
 * reads the offsets below.  tests/test_events.py pins the two against each
 * other. */
#define KOS_EVENT_ROW_SIZE 12u

/* One flag per process slot (PROCESS_TABLE_SIZE), indexed by pid. */
#define KOS_EVENT_IN_HANDLER_SIZE 7u

/* Offsets inside one row, matching the assembly's byte addressing. */
#define KOS_EVENT_OFF_OWNER      0u /* process slot; 0 marks a free row  */
#define KOS_EVENT_OFF_BANK       1u /* the owner's primary bank         */
#define KOS_EVENT_OFF_PENDING    2u /* events waiting for delivery      */
#define KOS_EVENT_OFF_MISSED     4u /* events dropped since the last one */
#define KOS_EVENT_OFF_TRAMPOLINE 6u /* libkos's event entry, in the owner */
#define KOS_EVENT_OFF_HANDLER    8u /* the application handler          */
#define KOS_EVENT_OFF_HANDLE    10u /* the bound handle the row serves  */
#define KOS_EVENT_OFF_RESERVED  11u

struct kos_event_row {
	uint8_t owner;      /* +0: the process slot, 0 when the row is free */
	uint8_t bank;       /* +1: the owner's primary bank */
	uint16_t pending;   /* +2 */
	uint16_t missed;    /* +4 */
	uint16_t trampoline;/* +6 */
	uint16_t handler;   /* +8 */
	uint8_t handle;     /* +10 */
	uint8_t reserved;   /* +11 */
};

/* --- common state (kernel/event/event_common.asm) ------------------------ */

extern struct kos_event_row kos_event_table[KOS_EVENT_MAX];
extern uint8_t kos_event_in_handler[KOS_EVENT_IN_HANDLER_SIZE];
/* The delivery frame of each process, and the exact latch it was built
 * under.  Both are per process: a handler may run a child that subscribes
 * and is delivered to as well, and the suspended handler's frame must
 * survive that. */
extern uint16_t kos_event_frame[KOS_EVENT_IN_HANDLER_SIZE];
extern uint8_t kos_event_frame_latch[KOS_EVENT_IN_HANDLER_SIZE];
/* The subscription being delivered, and the latch it is delivered under:
 * set by the return-path checks, read by pick and enter. */
extern uint16_t kos_event_row;
extern uint8_t kos_event_latch;

/* --- common primitives (kernel/event/event_common.asm) ------------------- */

/* The bank of the process the current context runs for: inside an interrupt
 * the interrupted code's bank (mode 10), otherwise the bank of the syscall
 * in progress.  `in_irq` is 1 inside an interrupt. */
uint8_t kos_event_post_bank(uint8_t in_irq);

/* The IRQ exit check and the syscall exit check: each returns with nothing
 * done, or never returns (the delivery unwinds to the interrupted code).
 * Called from the two gates' assembly, in common memory. */
void kos_event_irq_exit(void);
void kos_event_syscall_exit(void);

/* The frame EVENT_RETURN unwinds to: kos_gate_saved_sp + 2 (the trampoline's
 * RST return address sits directly below it). */
uint16_t kos_event_return_frame(void);

/* Pick the lowest deliverable row of the process kos_event_latch names and
 * record it in kos_event_row; returns the row address, or 0. */
uint16_t kos_event_pick(void);

/* Enter the picked row's handler: mark it active, snapshot and clear the
 * counters, select the application latch with the page cleared, and return
 * through RETI to the row's trampoline with BC = handler, HL = count,
 * DE = missed.  Never returns. */
void kos_event_enter(void);

/* Unwind: restore the latch in kos_event_latch, pop the frame at `frame`
 * and return to the interrupted code.  Never returns. */
void kos_event_unwind(uint16_t frame);

/* --- fixed-core body (kernel/event/event.c) ------------------------------ */

/* Zero the table and the per-process flags.  Common data is not part of the
 * boot's zeroed runtime areas, so the nucleus calls this once, before any
 * driver init can post. */
void kos_event_init(void);

/* One posted event for subscription `sub` (1..KOS_EVENT_MAX, the value the
 * driver was handed): the owner's bank must be `bank` for the event to
 * count as pending; otherwise it is dropped into `missed`.  Saturates at
 * FFFFh.  ISR-safe: it reads and writes the common table only. */
void kos_event_post(uint16_t sub, uint8_t bank);

/* --- cold-half bodies (kernel/event/event_syscall.c) --------------------- */

/* SYS_NOTIFY: subscribe (or replace the handler of) the running process on
 * the handle, or unsubscribe when the handler is 0.  The driver hears
 * KOS_OP_SUBSCRIBE with the subscription id, or 0 for an unsubscribe. */
void kos_event_notify(uint16_t block);

/* SYS_EVENT_RETURN: the trampoline's return from a handler.  Clears
 * `in_handler` and either delivers the next event through the same frame or
 * unwinds it.  Never returns. */
void kos_event_return(void);

/* Handle teardown (vfs_ns_release_entry): the (process, handle) row goes.
 * The driver hears KOS_OP_RELEASE through the VFS, not from here. */
void kos_event_handle_release(uint8_t pid, uint8_t handle);

/* Process exit (vfs_process_exit): every row and the handler flag go. */
void kos_event_process_exit(uint8_t pid);

#endif /* KONAOS_KERNEL_EVENT_EVENT_H */
