#ifndef KC_REMOTE_H
#define KC_REMOTE_H

#include "kc_protocol.h"

#define KC_PAGE_ENTRIES 20u
#define KC_REMOTE_STATUS_BASE 200

struct kc_entry {
	unsigned char flags;
	unsigned long size;
	char name[KC_MAX_NAME + 1u];
};

struct kc_page {
	unsigned long cursor;
	unsigned long next_cursor;
	unsigned char count;
	struct kc_entry entries[KC_PAGE_ENTRIES];
};

int kc_remote_hello(char *name, unsigned int capacity,
		    unsigned char *capabilities);
int kc_remote_list(const char *path, unsigned long cursor,
		   struct kc_page *page);
int kc_remote_open(const char *path, unsigned char *handle,
		   unsigned long *size);
int kc_remote_read(unsigned char handle, unsigned long offset,
		   unsigned char *data, unsigned int wanted,
		   unsigned int *received);
int kc_remote_close(unsigned char handle);
int kc_remote_create(const char *path, unsigned char *handle);
int kc_remote_write(unsigned char handle, unsigned long offset,
		    const unsigned char *data, unsigned int count);
int kc_remote_commit(unsigned char handle);
int kc_remote_abort(unsigned char handle);
const char *kc_remote_error(int error);

#endif

