#ifndef KONAEDIT_FILE_H
#define KONAEDIT_FILE_H

#include "ke_store.h"

#define KE_FILE_OK        0
#define KE_FILE_BINARY   -100
#define KE_FILE_TOO_LARGE -101
#define KE_FILE_PATH     -102

int ke_file_load(struct ke_store *store, const char *path);
int ke_file_save(struct ke_store *store, const char *path,
		 char *recovery, uint16_t recovery_size);

#endif
