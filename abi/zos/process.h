/* ZephyrOS native process ABI (public).
 *
 * The syscall-visible process contract shared by the kernel, modules and
 * native user programs: the startup block every native process receives,
 * the getinfo record, the descriptor flags the kernel publishes, and the
 * exec nesting limit.  The kernel's private process lifecycle interface
 * (process table, prepare/launch) stays in kernel/include/process.h,
 * which includes this header.
 *
 * Startup block v1 (package 28b) lives in the process's own page zero,
 * inside the caller-visible low window that stays mapped in both mode 10
 * and mode 11:
 *
 *   0080h  u8  version = ZOS_STARTUP_VERSION
 *   0081h  u8  tail_length  0..ZOS_STARTUP_TAIL_MAX
 *   0082h  ... tail bytes, then one NUL (always NUL-terminated)
 *
 * The kernel transports the tail byte-exact; user space owns parsing (the
 * SDK crt0, docs/sdk.md).  The launch gate enters the program with
 * HL = ZOS_STARTUP_BASE.  A program that ignores HL needs no glue.
 */

#ifndef ZOS_PROCESS_H
#define ZOS_PROCESS_H

#include "zos/types.h"

#define ZOS_STARTUP_BASE     0x0080u /* startup block address            */
#define ZOS_STARTUP_VERSION  1u      /* block version byte               */
#define ZOS_STARTUP_TAIL_MAX 125u    /* largest accepted args_length     */

/* The startup block as a C view over page zero at ZOS_STARTUP_BASE. */
struct zos_startup {
	uint8_t version;                              /* = 1            */
	uint8_t length;                               /* 0..TAIL_MAX    */
	char    tail[ZOS_STARTUP_TAIL_MAX + 1u];      /* NUL-terminated */
};

/* SYS_GETINFO writes a six-byte v1 record at the argument block and
 * returns 6 in HL.  Record flag bit 0 is set when the parent is the
 * kernel launcher (no user parent process). */
#define ZOS_GETINFO_SIZE 6u
#define ZOS_GETINFO_ABI  1u
#define ZOS_PROCESS_FLAG_KERNEL_PARENT 0x0001u

struct zos_info_record {
	uint8_t  size;  /* = ZOS_GETINFO_SIZE  */
	uint8_t  abi;   /* = ZOS_GETINFO_ABI   */
	uint8_t  pid;   /* process slot 0-6    */
	uint8_t  bank;  /* application bank 0-6 */
	uint16_t flags; /* ZOS_PROCESS_FLAG_*  */
};

/* Nesting geometry (package 28).  ZOS_EXEC_MAX_DEPTH user processes may
 * form one synchronous exec chain; the kernel launcher sits at depth 0.
 * The private frame-save geometry derived from this value lives in
 * kernel/include/process.h. */
#define ZOS_EXEC_MAX_DEPTH 4u

#if defined(__SDCC)
_Static_assert(ZOS_STARTUP_BASE + 2u + ZOS_STARTUP_TAIL_MAX + 1u == 0x0100u,
	       "the startup block must exactly fill 0080h-00FFh");
_Static_assert(sizeof(struct zos_startup) == 2u + ZOS_STARTUP_TAIL_MAX + 1u,
	       "the startup block view must match the page-zero layout");
_Static_assert(sizeof(struct zos_info_record) == ZOS_GETINFO_SIZE,
	       "getinfo record must be 6 packed bytes");
_Static_assert(__builtin_offsetof(struct zos_info_record, flags) == 4,
	       "getinfo flags offset");
#endif

#endif /* ZOS_PROCESS_H */
