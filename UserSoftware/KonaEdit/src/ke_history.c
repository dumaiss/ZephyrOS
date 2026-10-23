#if !defined(KE_HOST_TEST)
#include "kos.h"
#endif

#include "ke_history.h"
#include "ke_platform.h"

#define KE_HISTORY_OBJECT_SIZE   32768u
#define KE_CLIPBOARD_OBJECT_SIZE 16384u
#define KE_TRANSFER_SIZE         256u

static uint8_t transfer[KE_TRANSFER_SIZE];

static int area_read(const uint16_t *handles, ke_pos_t object_size,
		     ke_pos_t at, uint8_t *buffer, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		ke_pos_t absolute = at + done;
		uint8_t object = (uint8_t)(absolute / object_size);
		uint16_t offset = (uint16_t)(absolute % object_size);
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if ((ke_pos_t)chunk > object_size - offset)
			chunk = (uint16_t)(object_size - offset);
		result = ke_xmem_read(handles[object], offset, buffer + done, chunk);
		if (result != (int)chunk) return KE_HISTORY_ERROR;
		done = (uint16_t)(done + chunk);
	}
	return KE_HISTORY_OK;
}

static int area_write(const uint16_t *handles, ke_pos_t object_size,
		      ke_pos_t at, const uint8_t *buffer, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		ke_pos_t absolute = at + done;
		uint8_t object = (uint8_t)(absolute / object_size);
		uint16_t offset = (uint16_t)(absolute % object_size);
		uint16_t chunk = (uint16_t)(length - done);
		int result;

		if ((ke_pos_t)chunk > object_size - offset)
			chunk = (uint16_t)(object_size - offset);
		result = ke_xmem_write(handles[object], offset, buffer + done, chunk);
		if (result != (int)chunk) return KE_HISTORY_ERROR;
		done = (uint16_t)(done + chunk);
	}
	return KE_HISTORY_OK;
}

static void free_handles(uint16_t *handles, uint8_t count)
{
	uint8_t i;

	for (i = 0u; i < count; ++i) {
		if (handles[i] != 0u) {
			(void)ke_xmem_free(handles[i]);
			handles[i] = 0u;
		}
	}
}

int ke_history_init(struct ke_history *history)
{
	int handle;
	uint8_t i;

	if (history == (struct ke_history *)0) return KE_HISTORY_ERROR;
	for (i = 0u; i < 2u; ++i) {
		history->history_handle[i] = 0u;
		history->clipboard_handle[i] = 0u;
	}
	history->count = 0u;
	history->cursor = 0u;
	history->group = 0u;
	history->typing_length = 0u;
	history->used = 0ul;
	history->clipboard_length = 0ul;
	for (i = 0u; i < 2u; ++i) {
		handle = ke_xmem_alloc(KE_CLIPBOARD_OBJECT_SIZE,
				       KE_XMEM_MAPPABLE);
		if (handle < 0) goto fail;
		history->clipboard_handle[i] = (uint16_t)handle;
	}
	for (i = 0u; i < 2u; ++i) {
		handle = ke_xmem_alloc(KE_HISTORY_OBJECT_SIZE, KE_XMEM_NONE);
		if (handle < 0) goto fail;
		history->history_handle[i] = (uint16_t)handle;
	}
	return KE_HISTORY_OK;

fail:
	free_handles(history->clipboard_handle, 2u);
	free_handles(history->history_handle, 2u);
	return KE_HISTORY_ERROR;
}

void ke_history_dispose(struct ke_history *history)
{
	if (history == (struct ke_history *)0) return;
	free_handles(history->clipboard_handle, 2u);
	free_handles(history->history_handle, 2u);
	history->count = 0u;
	history->cursor = 0u;
	history->group = 0u;
	history->typing_length = 0u;
	history->used = 0ul;
	history->clipboard_length = 0ul;
}

void ke_history_clear(struct ke_history *history)
{
	history->count = 0u;
	history->cursor = 0u;
	history->group = 0u;
	history->typing_length = 0u;
	history->used = 0ul;
}

void ke_history_break_group(struct ke_history *history)
{
	history->group = 0u;
}

static int history_copy(struct ke_history *history, ke_pos_t destination,
			ke_pos_t source, ke_pos_t length)
{
	while (length != 0ul) {
		uint16_t chunk = length > KE_TRANSFER_SIZE ? KE_TRANSFER_SIZE :
				 (uint16_t)length;
		if (area_read(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			      source, transfer, chunk) != KE_HISTORY_OK ||
		    area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			       destination, transfer, chunk) != KE_HISTORY_OK)
			return KE_HISTORY_ERROR;
		source += chunk;
		destination += chunk;
		length -= chunk;
	}
	return KE_HISTORY_OK;
}

static int compact(struct ke_history *history)
{
	ke_pos_t destination = 0ul;
	uint8_t i;

	for (i = 0u; i < history->count; ++i) {
		struct ke_change *change = &history->changes[i];
		ke_pos_t bytes = change->old_length + change->new_length;

		if (change->payload != destination &&
		    history_copy(history, destination, change->payload, bytes) !=
			KE_HISTORY_OK) return KE_HISTORY_ERROR;
		change->payload = destination;
		destination += bytes;
	}
	history->used = destination;
	return KE_HISTORY_OK;
}

static int make_room(struct ke_history *history, ke_pos_t bytes)
{
	uint8_t i;

	if (bytes > KE_HISTORY_CAPACITY) return KE_HISTORY_UNRECORDED;
	if (history->cursor < history->count) {
		history->count = history->cursor;
		if (compact(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	}
	while (history->count >= KE_HISTORY_DEPTH ||
	       history->used + bytes > KE_HISTORY_CAPACITY) {
		for (i = 1u; i < history->count; ++i)
			history->changes[i - 1u] = history->changes[i];
		--history->count;
		if (history->cursor != 0u) --history->cursor;
		if (compact(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	}
	return KE_HISTORY_OK;
}

static int store_to_history(struct ke_history *history, struct ke_store *store,
			    ke_pos_t source, ke_pos_t destination,
			    ke_pos_t length)
{
	while (length != 0ul) {
		uint16_t chunk = length > KE_TRANSFER_SIZE ? KE_TRANSFER_SIZE :
				 (uint16_t)length;
		if (ke_store_read(store, source, transfer, chunk) != KE_STORE_OK ||
		    area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			       destination, transfer, chunk) != KE_HISTORY_OK)
			return KE_HISTORY_ERROR;
		source += chunk;
		destination += chunk;
		length -= chunk;
	}
	return KE_HISTORY_OK;
}

static int history_to_store(struct ke_history *history, struct ke_store *store,
			    ke_pos_t source, ke_pos_t destination,
			    ke_pos_t length)
{
	while (length != 0ul) {
		uint16_t chunk = length > KE_TRANSFER_SIZE ? KE_TRANSFER_SIZE :
				 (uint16_t)length;
		if (area_read(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			      source, transfer, chunk) != KE_HISTORY_OK ||
		    ke_store_insert(store, destination, transfer, chunk) != KE_STORE_OK)
			return KE_HISTORY_ERROR;
		source += chunk;
		destination += chunk;
		length -= chunk;
	}
	return KE_HISTORY_OK;
}

static int flush_typing(struct ke_history *history)
{
	if (history->typing_length == 0u) return KE_HISTORY_OK;
	if (area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
		       history->used - history->typing_length,
		       history->typing_stage, history->typing_length) != KE_HISTORY_OK)
		return KE_HISTORY_ERROR;
	history->typing_length = 0u;
	return KE_HISTORY_OK;
}

int ke_history_edit(struct ke_history *history, struct ke_store *store,
		    ke_pos_t position, ke_pos_t remove_length,
		    const uint8_t *insert, uint16_t insert_length)
{
	struct ke_change *change;
	ke_pos_t payload_size = remove_length + insert_length;
	int result;

	if (position > ke_store_length(store) ||
	    remove_length > ke_store_length(store) - position ||
	    ke_store_length(store) - remove_length + insert_length >
		KE_DOCUMENT_CAPACITY) return KE_HISTORY_ERROR;
	if (flush_typing(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	result = make_room(history, payload_size);
	if (result != KE_HISTORY_OK) return result;
	change = &history->changes[history->count];
	change->position = position;
	change->old_length = remove_length;
	change->new_length = insert_length;
	change->payload = history->used;
	if (store_to_history(history, store, position, change->payload,
			     remove_length) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	if (insert_length != 0u &&
	    area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
		       change->payload + remove_length, insert, insert_length) !=
		KE_HISTORY_OK) return KE_HISTORY_ERROR;
	if (remove_length != 0ul &&
	    ke_store_delete(store, position, remove_length) != KE_STORE_OK)
		return KE_HISTORY_ERROR;
	if (insert_length != 0u &&
	    ke_store_insert(store, position, insert, insert_length) != KE_STORE_OK)
		return KE_HISTORY_ERROR;
	history->used += payload_size;
	++history->count;
	history->cursor = history->count;
	history->group = 0u;
	history->typing_length = 0u;
	return KE_HISTORY_OK;
}

static int shift_right(struct ke_history *history, ke_pos_t at,
		       ke_pos_t length, uint16_t amount)
{
	while (length != 0ul) {
		uint16_t chunk = length > KE_TRANSFER_SIZE ? KE_TRANSFER_SIZE :
				 (uint16_t)length;
		ke_pos_t source = at + length - chunk;

		if (area_read(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			      source, transfer, chunk) != KE_HISTORY_OK ||
		    area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			       source + amount, transfer, chunk) != KE_HISTORY_OK)
			return KE_HISTORY_ERROR;
		length -= chunk;
	}
	return KE_HISTORY_OK;
}

int ke_history_edit_grouped(struct ke_history *history, struct ke_store *store,
			    ke_pos_t position, ke_pos_t remove_length,
			    const uint8_t *insert, uint16_t insert_length,
			    uint8_t group)
{
	struct ke_change *change;
	int result;

	if (history->group == group && history->cursor == history->count &&
	    history->count != 0u &&
	    history->used + remove_length + insert_length <=
		KE_HISTORY_CAPACITY) {
		change = &history->changes[history->count - 1u];
		if (remove_length == 0ul && change->old_length == 0ul &&
		    insert_length == 1u &&
		    position == change->position + change->new_length) {
			if (ke_store_insert(store, position, insert, insert_length) !=
				KE_STORE_OK) return KE_HISTORY_ERROR;
			change->new_length += insert_length;
			history->used += insert_length;
			history->typing_stage[history->typing_length++] = *insert;
			if (history->typing_length == KE_TYPING_STAGE &&
			    flush_typing(history) != KE_HISTORY_OK)
				return KE_HISTORY_ERROR;
			return KE_HISTORY_OK;
		}
		if (insert_length == 0u && change->new_length == 0ul &&
		    remove_length != 0ul && remove_length <= 65535ul &&
		    (position == change->position ||
		     position + remove_length == change->position)) {
			if (position < change->position &&
			    shift_right(history, change->payload,
					change->old_length,
					(uint16_t)remove_length) != KE_HISTORY_OK)
				return KE_HISTORY_ERROR;
			if (store_to_history(history, store, position,
				position < change->position ? change->payload :
				history->used, remove_length) != KE_HISTORY_OK ||
			    ke_store_delete(store, position, remove_length) !=
				KE_STORE_OK) return KE_HISTORY_ERROR;
			if (position < change->position) change->position = position;
			change->old_length += remove_length;
			history->used += remove_length;
			return KE_HISTORY_OK;
		}
	}
	if (group == KE_GROUP_TYPING && remove_length == 0ul &&
	    insert_length == 1u &&
	    position <= ke_store_length(store) &&
	    ke_store_length(store) + insert_length <= KE_DOCUMENT_CAPACITY) {
		if (flush_typing(history) != KE_HISTORY_OK)
			return KE_HISTORY_ERROR;
		result = make_room(history, insert_length);
		if (result != KE_HISTORY_OK) return result;
		change = &history->changes[history->count];
		change->position = position;
		change->old_length = 0ul;
		change->new_length = insert_length;
		change->payload = history->used;
		if (ke_store_insert(store, position, insert, insert_length) !=
		    KE_STORE_OK) return KE_HISTORY_ERROR;
		history->used += insert_length;
		++history->count;
		history->cursor = history->count;
		history->group = group;
		history->typing_stage[0] = *insert;
		history->typing_length = 1u;
		return KE_HISTORY_OK;
	}
	result = ke_history_edit(history, store, position, remove_length,
				 insert, insert_length);
	if (result == KE_HISTORY_OK) history->group = group;
	return result;
}

int ke_history_copy(struct ke_history *history, struct ke_store *store,
		    ke_pos_t position, ke_pos_t length)
{
	ke_pos_t done = 0ul;
	history->group = 0u;
	if (flush_typing(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;

	if (length > KE_CLIPBOARD_CAPACITY || position > ke_store_length(store) ||
	    length > ke_store_length(store) - position) return KE_HISTORY_ERROR;
	while (done < length) {
		uint16_t chunk = length - done > KE_TRANSFER_SIZE ? KE_TRANSFER_SIZE :
				 (uint16_t)(length - done);
		if (ke_store_read(store, position + done, transfer, chunk) !=
			KE_STORE_OK ||
		    area_write(history->clipboard_handle, KE_CLIPBOARD_OBJECT_SIZE,
			       done, transfer, chunk) != KE_HISTORY_OK)
			return KE_HISTORY_ERROR;
		done += chunk;
	}
	history->clipboard_length = length;
	return KE_HISTORY_OK;
}

int ke_history_cut(struct ke_history *history, struct ke_store *store,
		   ke_pos_t position, ke_pos_t length)
{
	int result = ke_history_copy(history, store, position, length);

	if (result != KE_HISTORY_OK) return result;
	return ke_history_edit(history, store, position, length,
			       (const uint8_t *)0, 0u);
}

int ke_history_paste(struct ke_history *history, struct ke_store *store,
		     ke_pos_t position)
{
	struct ke_change *change;
	ke_pos_t done = 0ul;
	int result;
	history->group = 0u;
	if (flush_typing(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;

	if (history->clipboard_length == 0ul) return KE_HISTORY_EMPTY;
	result = make_room(history, history->clipboard_length);
	if (result != KE_HISTORY_OK) return result;
	change = &history->changes[history->count];
	change->position = position;
	change->old_length = 0ul;
	change->new_length = history->clipboard_length;
	change->payload = history->used;
	while (done < history->clipboard_length) {
		uint16_t chunk = history->clipboard_length - done > KE_TRANSFER_SIZE ?
				 KE_TRANSFER_SIZE :
				 (uint16_t)(history->clipboard_length - done);
		if (area_read(history->clipboard_handle, KE_CLIPBOARD_OBJECT_SIZE,
			      done, transfer, chunk) != KE_HISTORY_OK ||
		    area_write(history->history_handle, KE_HISTORY_OBJECT_SIZE,
			       change->payload + done, transfer, chunk) != KE_HISTORY_OK ||
		    ke_store_insert(store, position + done, transfer, chunk) !=
			KE_STORE_OK) return KE_HISTORY_ERROR;
		done += chunk;
	}
	history->used += history->clipboard_length;
	++history->count;
	history->cursor = history->count;
	history->typing_length = 0u;
	return KE_HISTORY_OK;
}

int ke_history_undo(struct ke_history *history, struct ke_store *store)
{
	struct ke_change *change;

	history->group = 0u;
	if (flush_typing(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	if (history->cursor == 0u) return KE_HISTORY_EMPTY;
	change = &history->changes[history->cursor - 1u];
	if (change->new_length != 0ul &&
	    ke_store_delete(store, change->position, change->new_length) !=
		KE_STORE_OK) return KE_HISTORY_ERROR;
	if (history_to_store(history, store, change->payload, change->position,
			     change->old_length) != KE_HISTORY_OK)
		return KE_HISTORY_ERROR;
	--history->cursor;
	return KE_HISTORY_OK;
}

int ke_history_redo(struct ke_history *history, struct ke_store *store)
{
	struct ke_change *change;

	history->group = 0u;
	if (flush_typing(history) != KE_HISTORY_OK) return KE_HISTORY_ERROR;
	if (history->cursor >= history->count) return KE_HISTORY_EMPTY;
	change = &history->changes[history->cursor];
	if (change->old_length != 0ul &&
	    ke_store_delete(store, change->position, change->old_length) !=
		KE_STORE_OK) return KE_HISTORY_ERROR;
	if (history_to_store(history, store,
			     change->payload + change->old_length,
			     change->position, change->new_length) != KE_HISTORY_OK)
		return KE_HISTORY_ERROR;
	++history->cursor;
	return KE_HISTORY_OK;
}
