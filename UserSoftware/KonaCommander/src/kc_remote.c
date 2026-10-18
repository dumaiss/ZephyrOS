#include "kos.h"

#include "kc_protocol.h"
#include "kc_serial.h"
#include "kc_remote.h"

static struct kc_frame request;
static struct kc_frame response;
static char remote_message[97];

static unsigned int text_length(const char *text, unsigned int limit)
{
	unsigned int length = 0u;

	while (length < limit && text[length] != '\0') {
		++length;
	}
	return length;
}

static int exchange(unsigned char command, unsigned int payload_length)
{
	int result;
	unsigned int length;

	request.command = command;
	request.payload_length = payload_length;
	result = kc_link_request(&request, &response);
	if (result < 0) {
		return result;
	}
	if (response.status != KC_STATUS_OK) {
		length = response.payload_length;
		if (length > sizeof(remote_message) - 1u) {
			length = sizeof(remote_message) - 1u;
		}
		memcpy(remote_message, response.payload, length);
		remote_message[length] = '\0';
		return -(KC_REMOTE_STATUS_BASE + response.status);
	}
	remote_message[0] = '\0';
	return 0;
}

static int put_path(const char *path, unsigned int *used)
{
	unsigned int length = text_length(path, KC_MAX_PATH + 1u);

	if (length == 0u || length > KC_MAX_PATH || path[0] != '/') {
		return -KC_STATUS_BAD_REQUEST;
	}
	request.payload[0] = (unsigned char)length;
	memcpy(request.payload + 1u, path, length);
	*used = length + 1u;
	return 0;
}

int kc_remote_hello(char *name, unsigned int capacity,
		    unsigned char *capabilities)
{
	unsigned int name_length;
	int result = exchange(KC_CMD_HELLO, 0u);

	if (result < 0) {
		return result;
	}
	if (response.payload_length < 3u ||
	    kc_get_u16(response.payload) < KC_MAX_PAYLOAD) {
		return KC_LINK_EFRAME;
	}
	name_length = response.payload[2];
	if ((name_length + 3u != response.payload_length &&
	     name_length + 4u != response.payload_length) ||
	    name_length + 1u > capacity) {
		return KC_LINK_EFRAME;
	}
	memcpy(name, response.payload + 3u, name_length);
	name[name_length] = '\0';
	*capabilities = response.payload_length == name_length + 4u ?
		response.payload[3u + name_length] : 0u;
	return 0;
}

int kc_remote_list(const char *path, unsigned long cursor,
		   struct kc_page *page)
{
	unsigned int used;
	unsigned int offset;
	unsigned char index;
	int result = put_path(path, &used);

	if (result < 0) {
		return result;
	}
	kc_put_u32(request.payload + used, cursor);
	request.payload[used + 4u] = KC_PAGE_ENTRIES;
	result = exchange(KC_CMD_LIST, used + 5u);
	if (result < 0) {
		return result;
	}
	if (response.payload_length < 5u) {
		return KC_LINK_EFRAME;
	}
	page->cursor = cursor;
	page->next_cursor = kc_get_u32(response.payload);
	page->count = response.payload[4];
	if (page->count > KC_PAGE_ENTRIES) {
		return KC_LINK_EFRAME;
	}
	offset = 5u;
	for (index = 0u; index < page->count; ++index) {
		unsigned int name_length;
		struct kc_entry *entry = &page->entries[index];

		if (offset + 6u > response.payload_length) {
			return KC_LINK_EFRAME;
		}
		entry->flags = response.payload[offset++];
		entry->size = kc_get_u32(response.payload + offset);
		offset += 4u;
		name_length = response.payload[offset++];
		if (name_length == 0u || name_length > KC_MAX_NAME ||
		    offset + name_length > response.payload_length) {
			return KC_LINK_EFRAME;
		}
		memcpy(entry->name, response.payload + offset, name_length);
		entry->name[name_length] = '\0';
		offset += name_length;
	}
	return offset == response.payload_length ? 0 : KC_LINK_EFRAME;
}

int kc_remote_open(const char *path, unsigned char *handle,
		   unsigned long *size)
{
	unsigned int used;
	int result = put_path(path, &used);

	if (result < 0) {
		return result;
	}
	result = exchange(KC_CMD_OPEN, used);
	if (result < 0) {
		return result;
	}
	if (response.payload_length != 5u || response.payload[0] == 0u) {
		return KC_LINK_EFRAME;
	}
	*handle = response.payload[0];
	*size = kc_get_u32(response.payload + 1u);
	return 0;
}

int kc_remote_read(unsigned char handle, unsigned long offset,
		   unsigned char *data, unsigned int wanted,
		   unsigned int *received)
{
	unsigned int count;
	int result;

	if (wanted == 0u || wanted > KC_MAX_PAYLOAD - 6u) {
		return -KC_STATUS_TOO_LARGE;
	}
	request.payload[0] = handle;
	kc_put_u32(request.payload + 1u, offset);
	kc_put_u16(request.payload + 5u, wanted);
	result = exchange(KC_CMD_READ, 7u);
	if (result < 0) {
		return result;
	}
	if (response.payload_length < 6u ||
	    kc_get_u32(response.payload) != offset) {
		return KC_LINK_EFRAME;
	}
	count = kc_get_u16(response.payload + 4u);
	if (count > wanted || count + 6u != response.payload_length) {
		return KC_LINK_EFRAME;
	}
	memcpy(data, response.payload + 6u, count);
	*received = count;
	return 0;
}

int kc_remote_close(unsigned char handle)
{
	request.payload[0] = handle;
	return exchange(KC_CMD_CLOSE, 1u);
}

int kc_remote_create(const char *path, unsigned char *handle)
{
	unsigned int used;
	int result = put_path(path, &used);

	if (result < 0) return result;
	result = exchange(KC_CMD_CREATE, used);
	if (result < 0) return result;
	if (response.payload_length != 1u || response.payload[0] == 0u)
		return KC_LINK_EFRAME;
	*handle = response.payload[0];
	return 0;
}

int kc_remote_write(unsigned char handle, unsigned long offset,
		    const unsigned char *data, unsigned int count)
{
	int result;

	if (count == 0u || count > KC_MAX_PAYLOAD - 7u)
		return -KC_STATUS_TOO_LARGE;
	request.payload[0] = handle;
	kc_put_u32(request.payload + 1u, offset);
	kc_put_u16(request.payload + 5u, count);
	memcpy(request.payload + 7u, data, count);
	result = exchange(KC_CMD_WRITE, count + 7u);
	if (result < 0) return result;
	if (response.payload_length != 6u ||
	    kc_get_u32(response.payload) != offset ||
	    kc_get_u16(response.payload + 4u) != count)
		return KC_LINK_EFRAME;
	return 0;
}

int kc_remote_commit(unsigned char handle)
{
	request.payload[0] = handle;
	return exchange(KC_CMD_COMMIT, 1u);
}

int kc_remote_abort(unsigned char handle)
{
	request.payload[0] = handle;
	return exchange(KC_CMD_ABORT, 1u);
}

const char *kc_remote_error(int error)
{
	if (error <= -(KC_REMOTE_STATUS_BASE + 1) &&
	    error >= -(KC_REMOTE_STATUS_BASE + KC_STATUS_TOO_LARGE) &&
	    remote_message[0] != '\0') {
		return remote_message;
	}
	switch (error) {
	case KC_LINK_EDEVICE: return "serial device error";
	case KC_LINK_ETYPE: return "serial object has wrong type";
	case KC_LINK_ETIMER: return "timer device error";
	case KC_LINK_EWRITE: return "serial transmit failed";
	case KC_LINK_ETIMEOUT: return "PC server timeout";
	case KC_LINK_EFRAME: return "invalid KCP1 response";
	case KC_LINK_ECOBS: return "KCP1 framing error";
	default: return "remote operation failed";
	}
}
