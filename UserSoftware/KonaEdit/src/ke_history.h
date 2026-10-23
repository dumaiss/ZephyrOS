#ifndef KONAEDIT_HISTORY_H
#define KONAEDIT_HISTORY_H

#include "ke_store.h"

#define KE_HISTORY_DEPTH       3u
#define KE_HISTORY_CAPACITY    65536ul
#define KE_CLIPBOARD_CAPACITY  32768ul

#define KE_HISTORY_OK          0
#define KE_HISTORY_UNRECORDED  1
#define KE_HISTORY_EMPTY       2
#define KE_HISTORY_ERROR      -1

#define KE_GROUP_TYPING        1u
#define KE_GROUP_BACKSPACE     2u
#define KE_GROUP_DELETE        3u
#define KE_TYPING_STAGE       20u

struct ke_change {
	ke_pos_t position;
	ke_pos_t old_length;
	ke_pos_t new_length;
	ke_pos_t payload;
};

struct ke_history {
	uint16_t history_handle[2];
	uint16_t clipboard_handle[2];
	struct ke_change changes[KE_HISTORY_DEPTH];
	uint8_t count;
	uint8_t cursor;
	uint8_t group;
	uint8_t typing_length;
	uint8_t typing_stage[KE_TYPING_STAGE];
	ke_pos_t used;
	ke_pos_t clipboard_length;
};

int ke_history_init(struct ke_history *history);
void ke_history_dispose(struct ke_history *history);
void ke_history_clear(struct ke_history *history);
int ke_history_edit(struct ke_history *history, struct ke_store *store,
		    ke_pos_t position, ke_pos_t remove_length,
		    const uint8_t *insert, uint16_t insert_length);
int ke_history_edit_grouped(struct ke_history *history, struct ke_store *store,
			    ke_pos_t position, ke_pos_t remove_length,
			    const uint8_t *insert, uint16_t insert_length,
			    uint8_t group);
void ke_history_break_group(struct ke_history *history);
int ke_history_copy(struct ke_history *history, struct ke_store *store,
		    ke_pos_t position, ke_pos_t length);
int ke_history_cut(struct ke_history *history, struct ke_store *store,
		   ke_pos_t position, ke_pos_t length);
int ke_history_paste(struct ke_history *history, struct ke_store *store,
		     ke_pos_t position);
int ke_history_undo(struct ke_history *history, struct ke_store *store);
int ke_history_redo(struct ke_history *history, struct ke_store *store);

#endif
