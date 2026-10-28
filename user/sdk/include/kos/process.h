/* KonaOS SDK: process startup, argv and exec.
 *
 * crt0 saves the startup block the kernel delivers in HL
 * (KOS_STARTUP_BASE), copies its tail into the SDK's own buffer (the
 * page-zero block is not preserved across a syscall), splits that copy
 * and calls int main(int argc, char **argv) with argc <= KOS_ARGV_MAX.
 * argv holds the command-tail words only (the kernel transports no
 * program name).  A program that does not care about its arguments
 * writes int main(void).
 */

#ifndef KOS_SDK_PROCESS_H
#define KOS_SDK_PROCESS_H

#include "kos/types.h"
#include "kos/process.h"
#include "kos/syscall.h"

/* Largest argv crt0 stores. */
#define KOS_ARGV_MAX 16

/* The startup block delivered at entry, or NULL before crt0 initializes
 * it.  Provided for programs that inspect the raw block (e.g. the
 * filesystem acceptance program checks version 1 and an empty tail). */
struct kos_startup *startup(void);

/* Split `tail` in place into words and store up to `max` pointers in
 * `argv`; returns the total word count.  Spaces and tabs separate words;
 * a single quote makes everything up to the next one literal; a double
 * quote does the same except that `\"` and `\\` are escapes; outside
 * quotes `\x` is a literal `x`; adjacent runs join into one word, `''`
 * and `""` are one empty word, and an unterminated quote runs to the end
 * of the tail.  See docs/exe-format.md section 5. */
int splitargs(char *tail, char **argv, int max);

/* Wait for the next interrupt (HALT).  A program waiting on input calls
 * it between empty polls: it then polls once per interrupt - the timer
 * tick at least, 156.25 Hz - and the CPU is halted in between, which
 * keeps a polling loop's bus activity out of the audio band (the sound
 * card picks it up).  Events are delivered at the interrupt's return as
 * usual.  It halts only where an interrupt is sure to come - the system
 * has the timer (`/dev/timer`, asked once) and interrupts are enabled -
 * and returns at once otherwise. */
void idle(void);

/* An input wait's pacing: call idle_input() between empty input polls
 * and idle_input_ready() when input arrives.  While input is idle it
 * waits 16 interrupts before the next poll (about 10 polls a second),
 * keeping the poll's own bus activity out of the audio band; for about
 * a second after input it waits one, so typing responds per tick.  The
 * IO controller queues keys meanwhile, never losing one. */
void idle_input(void);
void idle_input_ready(void);

/* Run `path` synchronously with the raw argument tail `args` (NULL or ""
 * for none; over 125 bytes is refused locally with -KOS_EINVAL).
 * Returns the child's u16 exit status or -errno (docs/abi.md). */
int run(const char *path, const char *args);

/* Warm reboot: the kernel masks interrupts, quiesces the interrupt sources
 * and writes the reset latch, so the ROM reset vector re-copies the OS
 * exactly as at power-on.  Does not return. */
void reboot(void);

#endif /* KOS_SDK_PROCESS_H */
