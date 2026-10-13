/* KonaOS kernel-private interrupt-registry interface (kernel/irq).
 *
 * The driver-facing interrupt contract is kernel/include/irq_types.h: the
 * `irq_source_t`/`irq_handler_t` types, the logical source enum, and the
 * result codes.  A resident driver claims and releases sources only
 * through the bootstrap interface's `irq_claim`/`irq_release`
 * (drivers/include/driver.h).
 *
 * This header owns the executive lifecycle, the claim
 * checkpoint/rollback transaction and owner token, the dispatch
 * diagnostics, the platform quiesce hook, and the common-resident
 * interrupt-state lock.  Kernel subsystems include it from this explicit
 * owning path.
 */

#ifndef KONAOS_KERNEL_IRQ_PRIVATE_H
#define KONAOS_KERNEL_IRQ_PRIVATE_H

#include "irq_types.h"

/* Executive lifecycle. */
void irq_init(void);
int irq_claim(irq_source_t source, irq_handler_t handler, void *context);
int irq_release(irq_source_t source);

/* Claim transactions used around module/driver initialization.  A
 * checkpoint snapshots the claim serial; rollback releases exactly the
 * claims made after it (wrap-safe) and returns how many were released.
 * Claims made before the checkpoint are never touched.  The owner token
 * is recorded with each claim for diagnostics/ownership checks. */
typedef uint16_t irq_checkpoint_t;
irq_checkpoint_t irq_registry_checkpoint(void);
int irq_registry_rollback(irq_checkpoint_t checkpoint);
void irq_registry_set_owner(const void *owner);
const void *irq_registry_owner(void);

/* Dispatch diagnostics. */
uint16_t irq_unexpected_count(void);
uint16_t irq_unclaimed_count(void);
irq_source_t irq_last_source(void);

/* Production platform quiesce hook, implemented by the owning CTC
 * driver.  The CTC0 start/stop programming is acceptance-only and lives
 * under tests/target/. */
void platform_irq_quiesce(irq_source_t source);

/* Common-resident interrupt-state lock used by kernel subsystems that
 * mutate shared state (registry, drivers).  irq_arch_lock() disables
 * maskable interrupts and returns the previous IFF state;
 * irq_arch_unlock() re-enables only when that state was enabled. */
uint8_t irq_arch_lock(void);
void irq_arch_unlock(uint8_t enabled);
void irq_arch_init(void);

#endif /* KONAOS_KERNEL_IRQ_PRIVATE_H */
