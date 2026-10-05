/* ZephyrOS SDK: process startup, argv and exec.
 *
 * crt0 saves the startup block the kernel delivers in HL
 * (ZOS_STARTUP_BASE), copies its tail into the SDK's own buffer (the
 * page-zero block is not preserved across a syscall), splits that copy
 * and calls int main(int argc, char **argv) with argc <= ZOS_ARGV_MAX.
 * argv holds the command-tail words only (the kernel transports no
 * program name).  A program that does not care about its arguments
 * writes int main(void).
 */

#ifndef ZOS_SDK_PROCESS_H
#define ZOS_SDK_PROCESS_H

#include "zos/types.h"
#include "zos/process.h"
#include "zos/syscall.h"

/* Largest argv crt0 stores (work/28c-m9-user-sdk). */
#define ZOS_ARGV_MAX 16

/* The startup block delivered at entry, or NULL before crt0 initializes
 * it.  Provided for programs that inspect the raw block (e.g. the
 * filesystem acceptance program checks version 1 and an empty tail). */
struct zos_startup *zos_args(void);

/* Split `tail` in place into whitespace-separated words and store up to
 * `max` pointers in `argv`; returns the total word count.  Spaces/tabs
 * separate words; a double quote toggles a quoted section whose spaces
 * are literal and whose quotes are removed; adjacent runs join; an
 * unterminated quote runs to the end; an empty quoted word is an empty
 * argument.  See docs/zex-format.md section 5. */
int zos_split(char *tail, char **argv, int max);

/* Run `path` synchronously with the raw argument tail `args` (NULL or ""
 * for none; over 125 bytes is refused locally with -ZOS_EINVAL).
 * Returns the child's u16 exit status or -errno (docs/abi.md). */
int zos_exec(const char *path, const char *args);

#endif /* ZOS_SDK_PROCESS_H */
