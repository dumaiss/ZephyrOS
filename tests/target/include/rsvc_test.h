/* ZephyrOS resident-service spike: the evaluation IPC contract (test only).
 *
 * Shared by the client program (tests/target/rsvc/rsvc.c), the nucleus
 * substrate (tests/target/rsvc/rsvc_core.c) and the six synthetic
 * services.  This is the package-35 measurement contract, never the
 * native ABI: the system-call numbers 40h-47h are evaluated by the
 * nucleus dispatcher only while ZOS_RSVC_SPIKE is defined, and every
 * protocol constant below belongs to the spike.
 *
 * The frame is fixed at 32 bytes and copied at every crossing; a service
 * sees its own copy in the nucleus frame array, never the caller's
 * memory.  Buffer descriptors are nucleus-issued small ids; the address
 * behind an id is never visible to a service.
 */

#ifndef ZEPHYROS_TARGET_RSVC_TEST_H
#define ZEPHYROS_TARGET_RSVC_TEST_H

#include "zos/types.h"

/* --- evaluation-only syscall numbers (40h-47h) -------------------------- */

#define ZOS_RSVC_SYSCALL_LOOKUP    0x40u
#define ZOS_RSVC_SYSCALL_CALL      0x41u
#define ZOS_RSVC_SYSCALL_CALL_FULL 0x42u
#define ZOS_RSVC_SYSCALL_UPCALL    0x43u
#define ZOS_RSVC_SYSCALL_STACKS    0x44u
#define ZOS_RSVC_SYSCALL_EVIDENCE  0x45u
#define ZOS_RSVC_SYSCALL_PAINT     0x46u
#define ZOS_RSVC_SYSCALL_IRQ       0x47u

/* --- frame layout ------------------------------------------------------- */

#define RSVC_FRAME_MAX   32u   /* the frame is fixed at 32 bytes          */
#define RSVC_PAYLOAD_MAX 24u

/* Protocol flags (frame.flags). */
#define RSVC_FRAME_STAGED 0x01u /* stage the block through the provider   */
#define RSVC_FRAME_SPIN   0x02u /* keep the service domain open           */
#define RSVC_FRAME_STREAM 0x04u /* the object is an executable stream     */

/* Descriptor ids are 0..3; RSVC_DESC_NONE is the int8_t -1 encoding. */
#define RSVC_DESC_NONE 0xFFu

struct rsvc_frame {
	uint8_t op;                   /* protocol operation number       */
	uint8_t flags;                /* RSVC_FRAME_* protocol flags     */
	uint8_t desc;                 /* nucleus-issued descriptor id    */
	uint8_t arg0;                 /* protocol word 0                 */
	uint16_t arg1;                /* protocol word 1                 */
	uint8_t payload[RSVC_PAYLOAD_MAX]; /* inline payload words       */
	int16_t result;               /* service result, filled by the nucleus */
};

/* --- endpoint classes, operations and notes ----------------------------- */

#define RSVC_MAX_ENDPOINTS 8u

#define RSVC_CLASS_ECHO 1u
#define RSVC_CLASS_CON  2u
#define RSVC_CLASS_VFS  3u
#define RSVC_CLASS_FAT  4u
#define RSVC_CLASS_IOC  5u
#define RSVC_CLASS_IRQX 6u

#define RSVC_OP_ECHO_NULL     0u
#define RSVC_OP_ECHO_EXCHANGE 1u
#define RSVC_OP_ECHO_SELF     2u

#define RSVC_OP_CON_WRITE 0u
#define RSVC_OP_CON_POLL  1u
#define RSVC_OP_CON_GETC  2u
#define RSVC_OP_CON_PROBE 3u

#define RSVC_OP_VFS_READ    0u
#define RSVC_OP_VFS_SELF    1u
#define RSVC_OP_VFS_ON_EXIT 2u
#define RSVC_OP_VFS_EXEC    3u

#define RSVC_OP_FAT_READ 0u
#define RSVC_OP_IOC_READ 0u
#define RSVC_OP_IRQX_SPIN 0u

/* Endpoint registration flags. */
#define RSVC_EP_REENTRANT 0x01u /* a call into an active endpoint is run */
#define RSVC_EP_SAMEDOMAIN 0x02u /* the same-domain fast path is allowed */

/* Evidence counters (api->note). */
#define RSVC_NOTE_SERVICE_CALLS 0u
#define RSVC_NOTE_CHAIN_WORDS   1u
#define RSVC_NOTE_EXIT_NOTIFY   2u
#define RSVC_NOTE_STREAM_BYTES  3u
#define RSVC_NOTE_SERVICE_MARK  4u

/* --- block geometry ----------------------------------------------------- */

#define RSVC_BLOCK_BYTES  512u  /* the synthetic block the chain moves    */
#define RSVC_STREAM_BYTES 512u  /* the executable object the loader reads */
#define RSVC_STREAM_MAGIC 0x5Au /* first byte of an executable object     */
#define RSVC_OBJECT_HANDLE 1u   /* the object handle the nucleus holds    */

/* --- service API table (the nucleus-side implementation) ---------------- */

struct rsvc_api {
	int8_t (*register_ep)(uint8_t class, uint8_t instance,
			      uint16_t entry, uint8_t flags,
			      uint8_t version);
	int8_t (*lookup)(uint8_t class, uint8_t instance);
	int16_t (*call)(uint8_t handle, uint8_t op, struct rsvc_frame *frame);
	int16_t (*copy_in)(uint8_t desc, uint16_t dst, uint16_t length);
	int16_t (*copy_out)(uint8_t desc, uint16_t src, uint16_t length);
	int16_t (*move)(uint16_t src, uint16_t dst, uint16_t length);
	int16_t (*desc_info)(uint8_t id, uint8_t *direction);
	void (*console_putc)(uint8_t byte);
	int8_t (*console_poll)(void);
	int8_t (*console_getc)(void);
	int8_t (*irq_claim)(uint8_t source, uint16_t handler,
			    uint16_t context);
	void (*note)(uint8_t counter, uint16_t value);
};

/* --- syscall argument blocks -------------------------------------------- */

/* 40h LOOKUP {class, instance, handle, version}; succeeds with the
 * endpoint handle in HL, 0 when no endpoint matches. */
struct rsvc_lookup_req {
	uint8_t class;
	uint8_t instance;
	uint8_t handle;
	uint8_t version;
};

/* 41h/42h CALL {handle, buf_dir, frame_len, reserved, buf_addr, buf_len,
 * frame, result}.  `frame_len` is the protocol frame size (1..32);
 * `buf_addr`/`buf_len` name the caller's buffer window (0100h-1FFFh) and
 * `buf_dir` its direction (0 = in, 1 = out); the nucleus captures a
 * descriptor at call entry and discards it at exit.  The service's reply
 * frame and result land back in the block. */
struct rsvc_call_req {
	uint8_t handle;
	uint8_t buf_dir;
	uint8_t frame_len;
	uint8_t reserved;
	uint16_t buf_addr;
	uint16_t buf_len;
	struct rsvc_frame frame;
	int16_t result;
};

/* 43h UPCALL {kind, reserved, argument, result}: a nucleus -> service
 * call with no pathname and no frame exchange. */
#define RSVC_UPCALL_EXIT 0u
#define RSVC_UPCALL_EXEC 1u

struct rsvc_upcall_req {
	uint8_t kind;
	uint8_t reserved;
	uint16_t argument;
	int16_t result;
};

/* 47h IRQ {iterations, baseline, reserved}: run the IRQ-owning service's
 * spin with and without the service-domain handler. */
struct rsvc_irq_req {
	uint16_t iterations;
	uint8_t baseline;
	uint8_t reserved;
};

/* --- evidence block (45h EVIDENCE, 44h STACKS) -------------------------- */

struct rsvc_evidence {
	uint16_t calls;             /* service calls, all levels          */
	uint16_t nested_max;        /* deepest call depth reached         */
	uint16_t service_calls;     /* service dispatch entries           */
	uint16_t chain_words;       /* summed chain checksums             */
	uint16_t exit_notify;       /* VFS exit notifications             */
	uint16_t stream_bytes;      /* bytes the loader streamed          */
	uint16_t con_bytes;         /* console bytes written by `con`     */
	uint16_t irq_count;         /* IRQ trampoline entries             */
	uint16_t irq_in_domain;     /* trips taken with the domain open   */
	uint16_t reentrant_refused; /* refused re-entrant calls           */
	uint16_t depth_refused;     /* refused depth overflows            */
	uint16_t desc_forged;       /* forged descriptor ids              */
	uint16_t desc_refused;      /* refused descriptor captures        */
	uint16_t desc_captured;     /* descriptor ids issued              */
	uint16_t fast_path_calls;   /* same-domain fast-path invocations  */
	uint16_t relay_bytes;       /* bytes bounced through the nucleus  */
	uint16_t upcall_exit;       /* exit upcalls                       */
	uint16_t upcall_exec;       /* executable-stream upcalls          */
	uint16_t gate_sp_min;       /* lowest gate-stack SP observed      */
	uint16_t stack_service;     /* service-stack high-water mark      */
	uint16_t stack_gate;        /* gate-stack high-water mark         */
	uint16_t stack_isr;         /* ISR-stack high-water mark          */
	uint8_t endpoints;          /* registered endpoints               */
	uint8_t latch_ok;           /* last domain exit restored exactly  */
	uint8_t irq_baseline;       /* last IRQ run's baseline flag       */
	uint8_t reserved0;          /* last service mark                  */
};

#define ZOS_RSVC_EVIDENCE_SIZE ((uint16_t)sizeof(struct rsvc_evidence))

#if defined(__SDCC)
_Static_assert(sizeof(struct rsvc_frame) == RSVC_FRAME_MAX,
	       "the resident-service frame is fixed at 32 bytes");
#endif

#endif /* ZEPHYROS_TARGET_RSVC_TEST_H */
