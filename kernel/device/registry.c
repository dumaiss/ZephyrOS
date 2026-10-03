/* ZephyrOS M3 fixed-size device registry.
 *
 * The registry owns handles and the integrity metadata it needs; drivers
 * own their ops tables and private instance state.  There is no heap:
 * storage is a fixed 16-entry array in bank-7 kernel data, and handles
 * are issued as slot index + 1 (1..16, 0 invalid).
 *
 * All registry mutation runs with maskable interrupts disabled, using the
 * M2 executive lock helper, so a future IRQ-reachable reader can never
 * observe a half-updated entry.
 */

#include "zephyros/device.h"
#include "zephyros/irq.h"

struct zdevice device_table[ZDEV_MAX_DEVICES];

static uint8_t device_used[ZDEV_MAX_DEVICES];
static uint16_t device_generation[ZDEV_MAX_DEVICES];
static uint16_t device_next_generation;
static const void *device_current_owner;

static uint8_t device_name_equal(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		++a;
		++b;
	}
	return (uint8_t)(*a == *b);
}

void device_registry_init(void)
{
	uint8_t i;
	uint8_t enabled = irq_arch_lock();

	for (i = 0; i < ZDEV_MAX_DEVICES; ++i) {
		device_used[i] = 0;
		device_generation[i] = 0;
		device_table[i].handle = ZDEV_HANDLE_INVALID;
		device_table[i].class_id = 0;
		device_table[i].capabilities = 0;
		device_table[i].name = (const char *)0;
		device_table[i].ops = (const struct zdevice_ops *)0;
		device_table[i].state = (void *)0;
		device_table[i].driver = (const void *)0;
	}
	device_next_generation = 0;
	device_current_owner = (const void *)0;
	irq_arch_unlock(enabled);
}

int device_register(const struct zdevice_desc *desc, zdev_handle_t *out_handle)
{
	uint8_t i;
	uint8_t slot = ZDEV_MAX_DEVICES;
	uint8_t enabled;
	int result = ZDEV_OK;

	/* Reject null/structurally invalid descriptors before locking. */
	if (desc == (const struct zdevice_desc *)0 ||
	    desc->name == (const char *)0 ||
	    desc->name[0] == 0 ||
	    desc->ops == (const struct zdevice_ops *)0 ||
	    desc->class_id < ZDEV_SERIAL ||
	    desc->class_id > ZDEV_OTHER) {
		return ZDEV_EINVAL;
	}

	enabled = irq_arch_lock();

	/* Duplicate instance names are rejected before any slot is used. */
	for (i = 0; i < ZDEV_MAX_DEVICES; ++i) {
		if (device_used[i] &&
		    device_name_equal(device_table[i].name, desc->name)) {
			result = ZDEV_EBUSY;
			goto out;
		}
	}

	for (i = 0; i < ZDEV_MAX_DEVICES; ++i) {
		if (!device_used[i]) {
			slot = i;
			break;
		}
	}
	if (slot == ZDEV_MAX_DEVICES) {
		result = ZDEV_ENOSPC;
		goto out;
	}

	device_table[slot].handle = (zdev_handle_t)(slot + 1u);
	device_table[slot].class_id = desc->class_id;
	device_table[slot].capabilities = desc->capabilities;
	device_table[slot].name = desc->name;
	device_table[slot].ops = desc->ops;
	device_table[slot].state = desc->state;
	device_table[slot].driver = device_current_owner;
	device_generation[slot] = device_next_generation++;
	device_used[slot] = 1;

	if (out_handle != (zdev_handle_t *)0) {
		*out_handle = device_table[slot].handle;
	}

out:
	irq_arch_unlock(enabled);
	return result;
}

const struct zdevice *device_get(zdev_handle_t handle)
{
	if (handle == ZDEV_HANDLE_INVALID || handle > ZDEV_MAX_DEVICES) {
		return (const struct zdevice *)0;
	}
	if (!device_used[handle - 1u]) {
		return (const struct zdevice *)0;
	}
	return &device_table[handle - 1u];
}

zdev_handle_t device_find_capability(uint16_t required_caps,
				     zdev_handle_t after)
{
	uint8_t i;

	if (required_caps == 0 || after >= ZDEV_MAX_DEVICES) {
		return ZDEV_HANDLE_INVALID;
	}
	for (i = after; i < ZDEV_MAX_DEVICES; ++i) {
		if (device_used[i] &&
		    (device_table[i].capabilities & required_caps) ==
			required_caps) {
			return device_table[i].handle;
		}
	}
	return ZDEV_HANDLE_INVALID;
}

zdev_checkpoint_t device_registry_checkpoint(void)
{
	uint8_t enabled = irq_arch_lock();
	zdev_checkpoint_t checkpoint = device_next_generation;

	irq_arch_unlock(enabled);
	return checkpoint;
}

int device_registry_rollback(zdev_checkpoint_t checkpoint)
{
	uint8_t i;
	uint8_t enabled = irq_arch_lock();
	int removed = 0;

	for (i = 0; i < ZDEV_MAX_DEVICES; ++i) {
		/* Wrap-safe "generation >= checkpoint" comparison. */
		if (device_used[i] &&
		    (uint16_t)(device_generation[i] - checkpoint) < 0x8000u) {
			device_used[i] = 0;
			device_generation[i] = 0;
			device_table[i].handle = ZDEV_HANDLE_INVALID;
			device_table[i].class_id = 0;
			device_table[i].capabilities = 0;
			device_table[i].name = (const char *)0;
			device_table[i].ops = (const struct zdevice_ops *)0;
			device_table[i].state = (void *)0;
			device_table[i].driver = (const void *)0;
			++removed;
		}
	}
	irq_arch_unlock(enabled);
	return removed;
}

void device_registry_set_owner(const void *owner)
{
	device_current_owner = owner;
}

const void *device_registry_owner(void)
{
	return device_current_owner;
}
