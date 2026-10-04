/* ZephyrOS M8 integration acceptance selftest (test artifact).
 *
 * Runs from the late boot hook after the full production-style boot:
 * SIO + IOC + HID + the V9958 video/console device are all loaded from
 * the recovery store and the single console_bind_defaults() has just
 * selected the preferred HID-input / V9958-output pair.  The unchanged
 * M7 `monitor.zex` launcher runs after this test, so the interactive
 * session executes through the same binding.
 *
 * Control and outcome lines go through the direct early SIO: the
 * transcript exists while the normal console output is the V9958, which
 * is itself the early-SIO-independence proof.  The complete bulk
 * READY/DONE lifecycle and the scripted HID/V9958 monitor session are
 * replayed from these exact bytes by tests/test_m8_acceptance.py and
 * performed by the operator on hardware; this target covers the safe,
 * deterministic integration checks: class/capability discovery of all
 * four residents, the default binding, repeated PING exchanges, and a
 * battery of malformed/null/class/capability calls whose exact codes
 * and harmlessness (registry, module allocation, console directions)
 * are verified.
 */

#include "zephyros/console.h"
#include "zephyros/device.h"
#include "zephyros/hid.h"
#include "zephyros/ioc.h"
#include "zephyros/kernel.h"
#include "zephyros/module.h"
#include "zephyros/video.h"

#include "ioc_proto.h"
#include "m8_selftest.h"

#if ZOS_M8_SELFTEST

uint8_t m8_selftest_result;

static const char m8_digits[] = "0123456789ABCDEF";

/* Bulk probe buffer for rejected-sequence calls (never transferred). */
static uint8_t m8_bulk_probe[4];

/* Fields a well-behaved rejected call may never mutate. */
struct m8_snapshot {
	zdev_handle_t handle;
	uint8_t class_id;
	zdev_caps_t capabilities;
	const char *name;
	const void *char_ops;
	const void *class_ops;
	void *state;
};

static void m8_fail(uint8_t code, uint8_t detail)
{
	static const char prefix[] = "m8 selftest: FAIL 0x";
	static const char detail_prefix[] = " detail 0x";

	m8_selftest_result = code;
	early_console_puts(prefix);
	early_console_putc(m8_digits[(code >> 4) & 0x0Fu]);
	early_console_putc(m8_digits[code & 0x0Fu]);
	early_console_puts(detail_prefix);
	early_console_putc(m8_digits[(detail >> 4) & 0x0Fu]);
	early_console_putc(m8_digits[detail & 0x0Fu]);
	early_console_puts("\r\n");
}

static void m8_put_hex8(uint8_t value)
{
	early_console_putc(m8_digits[(value >> 4) & 0x0Fu]);
	early_console_putc(m8_digits[value & 0x0Fu]);
}

/* Bounded instance-name compare over the registry-owned string. */
static uint8_t m8_name_is(const char *name, const char *want)
{
	while (*want != '\0') {
		if (*name != *want) {
			return 0u;
		}
		++name;
		++want;
	}
	return (*name == '\0') ? 1u : 0u;
}

static void m8_put_name(const char *name)
{
	uint8_t i;

	for (i = 0u; i < 20u && name[i] != '\0'; ++i) {
		early_console_putc(name[i]);
	}
}

static uint8_t m8_device_count(void)
{
	uint8_t count = 0u;
	zdev_handle_t handle;

	for (handle = 1u; handle <= ZDEV_MAX_DEVICES; ++handle) {
		if (device_get(handle) != (const struct zdevice *)0) {
			++count;
		}
	}
	return count;
}

static void m8_snapshot_take(struct m8_snapshot *snap, zdev_handle_t handle)
{
	const struct zdevice *dev = device_get(handle);

	snap->handle = handle;
	if (dev == (const struct zdevice *)0) {
		snap->class_id = 0u;
		snap->capabilities = 0u;
		snap->name = (const char *)0;
		snap->char_ops = (const void *)0;
		snap->class_ops = (const void *)0;
		snap->state = (void *)0;
		return;
	}
	snap->class_id = dev->class_id;
	snap->capabilities = dev->capabilities;
	snap->name = dev->name;
	snap->char_ops = (const void *)dev->char_ops;
	snap->class_ops = dev->class_ops;
	snap->state = dev->state;
}

static uint8_t m8_snapshot_equal(const struct m8_snapshot *a,
				 const struct m8_snapshot *b)
{
	return (uint8_t)(a->handle == b->handle &&
			 a->class_id == b->class_id &&
			 a->capabilities == b->capabilities &&
			 a->name == b->name &&
			 a->char_ops == b->char_ops &&
			 a->class_ops == b->class_ops &&
			 a->state == b->state);
}

void zos_run_m8_selftest(void)
{
	zdev_handle_t sio;
	zdev_handle_t ioc;
	zdev_handle_t hid;
	zdev_handle_t video;
	const struct zdevice *dev;
	const void *class_ops = (const void *)0;
	void *state = (void *)0;
	const struct zioc_ops *ioc_ops;
	const struct zhid_ops *hid_ops;
	const struct zvideo_ops *video_ops;
	struct zioc_frame request;
	struct zioc_frame reply;
	struct m8_snapshot before[4];
	struct m8_snapshot after[4];
	static const char visible[] = "m8 selftest: visible console marker\r\n";
	uint8_t level;
	uint8_t pass;
	uint8_t i;
	uint8_t granules_before;
	uint16_t bits;
	int result;

	m8_selftest_result = ZOS_M8_FAIL_BOOT;

	/* Discovery is by class plus capabilities only; the exact instance
	 * names are checked after discovery because this image pins the
	 * full resident set. */
	sio = device_find(ZDEV_SERIAL, ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
			  0u);
	ioc = device_find(ZDEV_IOC, ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ |
					ZCAP_IOC_BULK_WRITE | ZCAP_IOC_STATUS,
			  0u);
	hid = device_find(ZDEV_HID, ZCAP_HID_TEXT | ZCAP_CONSOLE_IN, 0u);
	video = device_find(ZDEV_VIDEO, ZCAP_VIDEO_RESET |
					      ZCAP_VIDEO_PRESENT |
					      ZCAP_CHAR_OUT |
					      ZCAP_CONSOLE_OUT,
			    0u);
	if (sio == ZDEV_HANDLE_INVALID || ioc == ZDEV_HANDLE_INVALID ||
	    hid == ZDEV_HANDLE_INVALID || video == ZDEV_HANDLE_INVALID) {
		m8_fail(ZOS_M8_FAIL_BOOT, 0x01u);
		return;
	}

	/* The handles are issued in load order: the boot sequence sio ->
	 * ioc -> hid -> v9958 is therefore observable in the registry. */
	if (!(sio < ioc && ioc < hid && hid < video)) {
		m8_fail(ZOS_M8_FAIL_BOOT, 0x02u);
		return;
	}

	dev = device_get(sio);
	if (dev == (const struct zdevice *)0 ||
	    m8_name_is(dev->name, "sio0.b") == 0u ||
	    dev->class_id != ZDEV_SERIAL) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x01u);
		return;
	}
	dev = device_get(ioc);
	if (dev == (const struct zdevice *)0 ||
	    m8_name_is(dev->name, "ioc.0") == 0u ||
	    dev->class_id != ZDEV_IOC) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x02u);
		return;
	}
	dev = device_get(hid);
	if (dev == (const struct zdevice *)0 ||
	    m8_name_is(dev->name, "hid.keyboard0") == 0u ||
	    dev->class_id != ZDEV_HID) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x03u);
		return;
	}
	dev = device_get(video);
	if (dev == (const struct zdevice *)0 ||
	    m8_name_is(dev->name, "v9958.0") == 0u ||
	    dev->class_id != ZDEV_VIDEO) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x04u);
		return;
	}
	if (m8_device_count() != 4u) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x05u);
		return;
	}

	/* The one final console_bind_defaults() must have selected the
	 * preferred pair: HID input, V9958 output. */
	if (console_input_handle() != hid ||
	    console_output_handle() != video) {
		m8_fail(ZOS_M8_FAIL_BIND, 0x01u);
		return;
	}

	/* Early-SIO report lines (the transcript is produced while the
	 * normal console output is the V9958). */
	dev = device_get(hid);
	early_console_puts("m8 selftest: input ");
	m8_put_name(dev->name);
	early_console_puts(" class ");
	m8_put_hex8(dev->class_id);
	early_console_puts("\r\n");
	dev = device_get(video);
	early_console_puts("m8 selftest: output ");
	m8_put_name(dev->name);
	early_console_puts(" class ");
	m8_put_hex8(dev->class_id);
	early_console_puts("\r\n");
	early_console_puts("m8 selftest: devices 04\r\n");

	/* --- IOC class: interface, PING x2 with echo, semantic status --- */
	if (device_interface(ioc, ZDEV_IOC, &class_ops, &state) != ZDEV_OK ||
	    class_ops == (const void *)0 || state == (void *)0) {
		m8_fail(ZOS_M8_FAIL_IOC, 0x01u);
		return;
	}
	ioc_ops = (const struct zioc_ops *)class_ops;
	if (ioc_ops->command == 0 || ioc_ops->bulk_read == 0 ||
	    ioc_ops->bulk_write == 0 || ioc_ops->status == 0) {
		m8_fail(ZOS_M8_FAIL_IOC, 0x02u);
		return;
	}
	level = 0u;
	for (pass = 0u; pass < 2u; ++pass) {
		request.type = IOC_CMD_PING;
		request.status = 0u;
		request.length = IOC_PING_ECHO_BYTES;
		for (i = 0u; i < IOC_PING_ECHO_BYTES; ++i) {
			request.data[i] = (uint8_t)(0xA0u + i);
		}
		result = ioc_ops->command(state, &request, &reply);
		if (result != ZIOC_OK || reply.status != 0u ||
		    reply.type != IOC_RSP_PING ||
		    reply.length != IOC_PING_DATA_LEN) {
			m8_fail(ZOS_M8_FAIL_IOC,
				(uint8_t)(0x10u + pass));
			return;
		}
		for (i = 0u; i < IOC_PING_ECHO_BYTES; ++i) {
			if (reply.data[i] != (uint8_t)(0xA0u + i)) {
				m8_fail(ZOS_M8_FAIL_IOC,
					(uint8_t)(0x20u + pass));
				return;
			}
		}
		if ((reply.data[IOC_PING_DATA_POWER] &
		     IOC_PING_LINK_SYNCED) == 0u) {
			m8_fail(ZOS_M8_FAIL_IOC,
				(uint8_t)(0x30u + pass));
			return;
		}
		if (pass == 0u) {
			level = reply.data[IOC_PING_DATA_LEVEL];
		} else if (reply.data[IOC_PING_DATA_LEVEL] != level) {
			m8_fail(ZOS_M8_FAIL_IOC, 0x31u);
			return;
		}
	}
	early_console_puts("m8 selftest: ioc level ");
	m8_put_hex8(level);
	early_console_puts("\r\n");
	bits = ioc_ops->status(state);
	if ((bits & (uint16_t)~ZIOC_STATUS_HID_PENDING) != 0u) {
		m8_fail(ZOS_M8_FAIL_IOC, 0x32u);
		return;
	}

	/* --- HID class shape: text present, gamepad reserved/null ------- */
	if (device_interface(hid, ZDEV_HID, &class_ops, &state) != ZDEV_OK ||
	    class_ops == (const void *)0 || state == (void *)0) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x10u);
		return;
	}
	hid_ops = (const struct zhid_ops *)class_ops;
	if (hid_ops->read_text == 0 || hid_ops->read_gamepad != 0) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x11u);
		return;
	}
	dev = device_get(hid);
	if ((dev->capabilities & ZCAP_HID_GAMEPAD) != 0u) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x12u);
		return;
	}

	/* --- VIDEO class shape and safe operation rejections ------------ */
	if (device_interface(video, ZDEV_VIDEO, &class_ops, &state) !=
	    ZDEV_OK ||
	    class_ops == (const void *)0 || state == (void *)0) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x20u);
		return;
	}
	video_ops = (const struct zvideo_ops *)class_ops;
	if (video_ops->send == 0) {
		m8_fail(ZOS_M8_FAIL_SHAPE, 0x21u);
		return;
	}

	/* Snapshot everything a rejected call must leave alone. */
	m8_snapshot_take(&before[0], sio);
	m8_snapshot_take(&before[1], ioc);
	m8_snapshot_take(&before[2], hid);
	m8_snapshot_take(&before[3], video);
	granules_before = module_granule_count();

	/* --- malformed/null/class/capability calls, exact codes --------- */
	if (device_find(ZDEV_HID, 0u, 0u) != ZDEV_HANDLE_INVALID ||
	    device_find(0x0Fu, ZCAP_CHAR_IN, 0u) != ZDEV_HANDLE_INVALID ||
	    device_find(ZDEV_SERIAL, ZCAP_CONSOLE_IN,
			(zdev_handle_t)(ZDEV_MAX_DEVICES + 1u)) !=
		    ZDEV_HANDLE_INVALID) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x01u);
		return;
	}
	if (device_interface(ioc, ZDEV_HID, &class_ops, &state) !=
	    ZDEV_ENOTSUP ||
	    device_interface(ZDEV_HANDLE_INVALID, ZDEV_HID, &class_ops,
			     &state) != ZDEV_EINVAL ||
	    device_interface(hid, ZDEV_HID, (const void **)0, (void **)0) !=
		    ZDEV_EINVAL) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x02u);
		return;
	}
	if (video_ops->send(state, 0x40u, (const void *)0, 0u) !=
	    ZDEV_ENOTSUP ||
	    video_ops->send((void *)0, ZVIDEO_PRESENT, (const void *)0, 0u) !=
		    ZDEV_ENODEV ||
	    video_ops->send(state, ZVIDEO_PRESENT, (const void *)0, 5u) !=
		    ZDEV_EINVAL) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x03u);
		return;
	}
	/* Bulk calls before any READY admission are refused without any
	 * lane I/O; an oversized command DATA length is refused before
	 * the request is built. */
	if (ioc_ops->bulk_read(state, m8_bulk_probe, 4u) != ZIOC_EBUSY ||
	    ioc_ops->bulk_write(state, m8_bulk_probe, 4u) != ZIOC_EBUSY) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x04u);
		return;
	}
	request.type = IOC_CMD_PING;
	request.status = 0u;
	request.length = (uint8_t)(ZIOC_MAX_DATA + 1u);
	result = ioc_ops->command(state, &request, &reply);
	if (result != ZIOC_EINVAL) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x05u);
		return;
	}
	/* A console_bind that cannot work must be rejected and must not
	 * disturb the current binding. */
	if (console_bind(ioc, video) != ZDEV_EINVAL ||
	    console_bind(hid, ioc) != ZDEV_EINVAL ||
	    console_input_handle() != hid ||
	    console_output_handle() != video) {
		m8_fail(ZOS_M8_FAIL_MALFORMED, 0x06u);
		return;
	}

	/* --- invariants: device table, allocation, console service ----- */
	m8_snapshot_take(&after[0], sio);
	m8_snapshot_take(&after[1], ioc);
	m8_snapshot_take(&after[2], hid);
	m8_snapshot_take(&after[3], video);
	if (m8_device_count() != 4u ||
	    module_granule_count() != granules_before ||
	    m8_snapshot_equal(&before[0], &after[0]) == 0u ||
	    m8_snapshot_equal(&before[1], &after[1]) == 0u ||
	    m8_snapshot_equal(&before[2], &after[2]) == 0u ||
	    m8_snapshot_equal(&before[3], &after[3]) == 0u) {
		m8_fail(ZOS_M8_FAIL_INVARIANTS, 0x01u);
		return;
	}
	result = console_write(visible, (uint16_t)(sizeof(visible) - 1u));
	if (result != (int)(sizeof(visible) - 1u)) {
		m8_fail(ZOS_M8_FAIL_CONSOLE, 0x01u);
		return;
	}
	result = console_poll();
	if (result < 0 || result > 1) {
		m8_fail(ZOS_M8_FAIL_CONSOLE, 0x02u);
		return;
	}
	early_console_puts("m8 selftest: rejected calls OK\r\n");

	m8_selftest_result = ZOS_M8_OK;
	early_console_puts("m8 selftest: OK\r\n");
}

#endif /* ZOS_M8_SELFTEST */
