/* ZephyrOS native startup-block runtime (user/runtime, package 28b).
 *
 * The kernel enters every native program with HL pointing at its startup
 * block v1 in page zero.  A program that wants its arguments calls
 * zos_args_init() once, before anything else clobbers HL (the entry glue
 * in the program's first _CODE object is the natural place); it stores HL
 * and returns it unchanged.  Programs that ignore HL need none of this
 * and do not link this module.
 *
 * Block layout (docs/zex-format.md section 5, include/zephyros/syscall.h):
 *
 *   0080h  u8  version = ZEX_STARTUP_VERSION
 *   0081h  u8  length  0..ZEX_STARTUP_TAIL_MAX
 *   0082h  ... length raw bytes, then one NUL
 *
 * The kernel transports the tail byte-exact and never parses it.  Splitting
 * is runtime policy: zos_split() is a small, documented, replaceable
 * user-space helper, not an ABI.
 */

#ifndef ZEPHYROS_USER_ZEX_ARGS_RUNTIME_H
#define ZEPHYROS_USER_ZEX_ARGS_RUNTIME_H

/* Mirrors include/zephyros/syscall.h (the runtime headers do not depend on
 * the kernel ABI header). */
#define ZEX_STARTUP_BASE     0x0080
#define ZEX_STARTUP_VERSION  1
#define ZEX_STARTUP_TAIL_MAX 125

struct zex_startup {
	unsigned char version;                 /* = ZEX_STARTUP_VERSION */
	unsigned char length;                  /* 0..ZEX_STARTUP_TAIL_MAX */
	char          tail[ZEX_STARTUP_TAIL_MAX + 1]; /* NUL-terminated */
};

/* Save the startup block pointer (HL) delivered at entry.  Implemented in
 * zex_args.asm; preserves HL. */
void zos_args_init(void);

/* The saved startup block, or NULL when zos_args_init() was not called. */
struct zex_startup *zos_args(void);

/* Split the tail in place into whitespace-separated words and store up to
 * `max` pointers in `argv`.  Returns the total number of words; only the
 * first `max` are stored, so a caller can detect truncation by comparing
 * the result with `max`.  The exact rules are documented in
 * docs/zex-format.md section 5: spaces/tabs separate words; a double
 * quote toggles a quoted section whose spaces are literal; quotes are
 * removed; adjacent quoted and unquoted runs join; an unterminated quote
 * runs to the end of the tail; an empty quoted word is an empty argument.
 * A NUL byte ends the tail.  `tail` must be writable. */
int zos_split(char *tail, char **argv, int max);

#endif /* ZEPHYROS_USER_ZEX_ARGS_RUNTIME_H */
