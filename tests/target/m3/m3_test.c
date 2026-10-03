/* ZephyrOS M3 target acceptance test (test artifact).
 *
 * Runs the M3 acceptance surface against the real kernel code paths:
 *
 *   - registry semantics: invalid/duplicate descriptors, handle validity,
 *     capability iteration, transaction checkpoint/rollback, exhaustion
 *     without overwriting existing entries,
 *   - console service: TX, capability-based binding, unsupported
 *     operations, unbound errors, and RX of a byte sent by the operator
 *     (or the host replay),
 *   - M2 integration: the test claims IRQ_CTC0 directly through the M2
 *     executive and receives repeated deliveries on its bank-7 handler.
 *
 * TX/RX/IRQ progress is observable on the serial line; the final result
 * is also stored in m3_selftest_result for the host replay.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/kernel.h"
#include "zephyros/platform.h"

#include "m3_selftest.h"

extern int m3_irq_probe_run(void);
extern const struct zdevice_ops m3_fixture_ops;
extern const struct zdevice_ops m3_fixture_caponly_ops;

volatile uint8_t m3_selftest_result;
volatile uint16_t m3_selftest_pacing;

static const char m3_hex_digits[] = "0123456789ABCDEF";

static const char m3_msg_tx[] = "m3 selftest: console TX OK\r\n";
static const char m3_msg_rx_ready[] = "m3 selftest: ready for RX\r\n";
static const char m3_msg_rx_ok[] = "m3 selftest: RX OK\r\n";

static const char *const m3_fill_names[ZDEV_MAX_DEVICES] = {
	"test.fill.00", "test.fill.01", "test.fill.02", "test.fill.03",
	"test.fill.04", "test.fill.05", "test.fill.06", "test.fill.07",
	"test.fill.08", "test.fill.09", "test.fill.10", "test.fill.11",
	"test.fill.12", "test.fill.13", "test.fill.14", "test.fill.15"
};

static void m3_fail(uint8_t code)
{
	m3_selftest_result = code;
	early_console_puts("m3 selftest: FAIL code=0x");
	early_console_putc(m3_hex_digits[code >> 4]);
	early_console_putc(m3_hex_digits[code & 0x0Fu]);
	early_console_puts("\r\n");
}

static uint8_t m3_name_equal(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		++a;
		++b;
	}
	return (uint8_t)(*a == *b);
}

static zdev_handle_t m3_find_by_name(const char *name)
{
	zdev_handle_t handle;

	for (handle = 1; handle <= ZDEV_MAX_DEVICES; ++handle) {
		const struct zdevice *dev = device_get(handle);

		if (dev != (const struct zdevice *)0 &&
		    m3_name_equal(dev->name, name)) {
			return handle;
		}
	}
	return ZDEV_HANDLE_INVALID;
}

static int m3_register_named(const char *name, uint16_t caps,
			     const struct zdevice_ops *ops)
{
	struct zdevice_desc desc;

	desc.class_id = ZDEV_OTHER;
	desc.capabilities = caps;
	desc.name = name;
	desc.ops = ops;
	desc.state = (void *)0;
	return device_register(&desc, (zdev_handle_t *)0);
}

static void m3_valid_desc(struct zdevice_desc *desc)
{
	desc->class_id = ZDEV_OTHER;
	desc->capabilities = 0;
	desc->name = "test.desc";
	desc->ops = &m3_fixture_ops;
	desc->state = (void *)0;
}

void zos_run_m3_selftest(void)
{
	zdev_handle_t sio_in;
	zdev_handle_t sio_out;
	zdev_handle_t handle;
	const struct zdevice *dev;
	zdev_checkpoint_t checkpoint;
	struct zdevice_desc desc;
	uint16_t expected;
	uint16_t attempt;
	uint16_t spin;
	uint8_t i;
	int rc;

	m3_selftest_result = ZOS_M3ST_OK;

	/* --- 1. registry + capability-based default binding --------------- */
	if (device_get(ZDEV_HANDLE_INVALID) != (const struct zdevice *)0) {
		m3_fail(ZOS_M3ST_FAIL_REGISTRY); return;
	}
	if (device_get(ZDEV_MAX_DEVICES + 1u) != (const struct zdevice *)0) {
		m3_fail(ZOS_M3ST_FAIL_REGISTRY); return;
	}
	sio_out = device_find_capability(ZCAP_CONSOLE_OUT, 0);
	sio_in = device_find_capability(ZCAP_CONSOLE_IN, 0);
	if (sio_out == ZDEV_HANDLE_INVALID || sio_in == ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	dev = device_get(sio_out);
	if (dev == (const struct zdevice *)0 ||
	    !m3_name_equal(dev->name, "sio0.b") ||
	    dev->class_id != ZDEV_SERIAL) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	if ((dev->capabilities & (ZCAP_CHAR_IN | ZCAP_CHAR_OUT |
				  ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT)) !=
	    (ZCAP_CHAR_IN | ZCAP_CHAR_OUT |
	     ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT)) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	if (device_find_capability(ZCAP_CHAR_IN | ZCAP_CONSOLE_IN, 0) ==
	    ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	if (device_find_capability(ZCAP_CONSOLE_OUT, sio_out) !=
	    ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	if (device_find_capability(0, 0) != ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}
	if (console_input_handle() != sio_in ||
	    console_output_handle() != sio_out) {
		m3_fail(ZOS_M3ST_FAIL_CAPABILITY); return;
	}

	/* --- 2. duplicates and invalid descriptors ------------------------ */
	if (m3_register_named("sio0.b", ZCAP_CHAR_OUT, &m3_fixture_ops) !=
	    ZDEV_EBUSY) {
		m3_fail(ZOS_M3ST_FAIL_DUPLICATE); return;
	}
	if (device_register((const struct zdevice_desc *)0,
			    (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}
	m3_valid_desc(&desc);
	desc.name = (const char *)0;
	if (device_register(&desc, (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}
	desc.name = "";
	if (device_register(&desc, (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}
	desc.name = "test.desc";
	desc.ops = (const struct zdevice_ops *)0;
	if (device_register(&desc, (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}
	desc.ops = &m3_fixture_ops;
	desc.class_id = 0;
	if (device_register(&desc, (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}
	desc.class_id = 99;
	if (device_register(&desc, (zdev_handle_t *)0) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_INVALID); return;
	}

	/* --- 3. checkpoint/rollback transaction --------------------------- */
	checkpoint = device_registry_checkpoint();
	if (m3_register_named("test.tx.a", 0, &m3_fixture_ops) != ZDEV_OK ||
	    m3_register_named("test.tx.b", 0, &m3_fixture_ops) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_REGISTRY); return;
	}
	if (device_registry_rollback(checkpoint) != 2) {
		m3_fail(ZOS_M3ST_FAIL_ROLLBACK); return;
	}
	if (m3_find_by_name("test.tx.a") != ZDEV_HANDLE_INVALID ||
	    m3_find_by_name("test.tx.b") != ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_ROLLBACK); return;
	}
	if (m3_find_by_name("sio0.b") == ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_ROLLBACK); return;
	}
	if (m3_register_named("test.tx.c", 0, &m3_fixture_ops) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_ROLLBACK); return;
	}
	if (device_registry_rollback(checkpoint) != 1) {
		m3_fail(ZOS_M3ST_FAIL_ROLLBACK); return;
	}

	/* --- 4. exhaustion must not overwrite existing entries ------------ */
	checkpoint = device_registry_checkpoint();
	for (i = 0; i < ZDEV_MAX_DEVICES; ++i) {
		rc = m3_register_named(m3_fill_names[i], 0, &m3_fixture_ops);
		if (i < ZDEV_MAX_DEVICES - 1u) {
			if (rc != ZDEV_OK) {
				m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
			}
		} else if (rc != ZDEV_ENOSPC) {
			m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
		}
	}
	dev = device_get(sio_out);
	if (dev == (const struct zdevice *)0 ||
	    !m3_name_equal(dev->name, "sio0.b") ||
	    (dev->capabilities & ZCAP_CONSOLE_OUT) == 0) {
		m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
	}
	if (m3_find_by_name("test.fill.00") == ZDEV_HANDLE_INVALID ||
	    m3_find_by_name("test.fill.15") != ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
	}
	if (device_registry_rollback(checkpoint) !=
	    (int)(ZDEV_MAX_DEVICES - 1u)) {
		m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
	}
	if (m3_find_by_name("test.fill.00") != ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_EXHAUSTION); return;
	}

	/* --- 5. console service: TX, errors, binding ---------------------- */
	expected = (uint16_t)(sizeof(m3_msg_tx) - 1u);
	if (console_write(m3_msg_tx, expected) != (int)expected) {
		m3_fail(ZOS_M3ST_FAIL_TX); return;
	}
	if (m3_register_named("test.caponly",
			      ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
			      &m3_fixture_caponly_ops) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	handle = m3_find_by_name("test.caponly");
	if (handle == ZDEV_HANDLE_INVALID) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (console_bind(ZDEV_HANDLE_INVALID, handle) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (console_putc('x') != ZDEV_ENOTSUP ||
	    console_write("x", 1) != ZDEV_ENOTSUP) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (console_bind(handle, ZDEV_HANDLE_INVALID) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (console_poll() != ZDEV_ENOTSUP ||
	    console_getc() != ZDEV_ENOTSUP) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (m3_register_named("test.plainout", ZCAP_CHAR_OUT,
			      &m3_fixture_ops) != ZDEV_OK) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	handle = m3_find_by_name("test.plainout");
	if (handle == ZDEV_HANDLE_INVALID ||
	    console_bind(ZDEV_HANDLE_INVALID, handle) != ZDEV_EINVAL ||
	    console_bind(ZDEV_HANDLE_INVALID, 99) != ZDEV_EINVAL ||
	    console_bind(handle, ZDEV_HANDLE_INVALID) != ZDEV_EINVAL) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	console_unbind();
	if (console_putc('x') != ZDEV_ENODEV ||
	    console_write("x", 1) != ZDEV_ENODEV ||
	    console_poll() != ZDEV_ENODEV ||
	    console_getc() != ZDEV_ENODEV) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}
	if (console_bind_defaults() != ZDEV_OK ||
	    console_input_handle() != sio_in ||
	    console_output_handle() != sio_out) {
		m3_fail(ZOS_M3ST_FAIL_CONSOLE); return;
	}

	/* --- 6. console RX: bounded poll for the operator's byte ---------- */
	expected = (uint16_t)(sizeof(m3_msg_rx_ready) - 1u);
	if (console_write(m3_msg_rx_ready, expected) != (int)expected) {
		m3_fail(ZOS_M3ST_FAIL_RX); return;
	}
	rc = ZDEV_EAGAIN;
	for (attempt = 0; attempt < ZOS_M3ST_RX_ATTEMPTS; ++attempt) {
		if (console_poll() == 1) {
			rc = console_getc();
			if (rc == (int)ZOS_M3ST_RX_BYTE) {
				break;
			}
			m3_fail(ZOS_M3ST_FAIL_RX_BYTE); return;
		}
		/* Short pacing delay so the window spans seconds on
		 * 10 MHz hardware without a timer. */
		for (spin = 0; spin < 0x0400u; ++spin) {
			++m3_selftest_pacing;
		}
	}
	if (rc != (int)ZOS_M3ST_RX_BYTE) {
		m3_fail(ZOS_M3ST_FAIL_RX); return;
	}
	expected = (uint16_t)(sizeof(m3_msg_rx_ok) - 1u);
	if (console_write(m3_msg_rx_ok, expected) != (int)expected) {
		m3_fail(ZOS_M3ST_FAIL_RX); return;
	}

	/* --- 7. direct CTC0 IRQ path through the M2 executive ------------- */
	if (m3_irq_probe_run() != 0) {
		m3_fail(ZOS_M3ST_FAIL_IRQ); return;
	}

	m3_selftest_result = ZOS_M3ST_OK;
	early_console_puts("m3 selftest: OK\r\n");
}
