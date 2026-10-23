#if defined(KE_HOST_TEST)
#define KOS_EIO 10
#else
#include "kos.h"
#endif

#include "ke_file.h"
#include "ke_platform.h"
#include "ke_text.h"

#define KE_IO_SIZE 512u

/* Loading and saving are synchronous and never nest, so one transfer buffer
 * serves both paths without reducing their 512-byte I/O chunk. */
static uint8_t io_buffer[KE_IO_SIZE];
#define input_buffer io_buffer
#define output_buffer io_buffer
static char temporary_path[KE_PATH_MAX];
static char backup_path[KE_PATH_MAX];

static uint16_t text_length(const char *text, uint16_t limit)
{
	uint16_t length = 0u;

	if (text == (const char *)0) return limit;
	while (length < limit && text[length] != '\0') ++length;
	return length;
}

static int sibling_path(const char *path, const char *leaf, char *result)
{
	uint16_t length = text_length(path, KE_PATH_MAX);
	uint16_t leaf_length = text_length(leaf, KE_PATH_MAX);
	uint16_t slash = 0u;
	uint16_t i;

	if (length == 0u || length >= KE_PATH_MAX ||
	    leaf_length == 0u || leaf_length >= KE_PATH_MAX) return KE_FILE_PATH;
	for (i = 0u; i < length; ++i) {
		if (path[i] == '/') slash = (uint16_t)(i + 1u);
	}
	if (slash + leaf_length >= KE_PATH_MAX) return KE_FILE_PATH;
	for (i = 0u; i < slash; ++i) result[i] = path[i];
	for (i = 0u; i < leaf_length; ++i) result[slash + i] = leaf[i];
	result[slash + leaf_length] = '\0';
	return KE_FILE_OK;
}

static int unique_path(const char *target, char *path, uint8_t backup,
		       int *handle)
{
	char leaf[13] = "KETMP0.$$$";
	uint8_t attempt;
	struct ke_file_info info;

	if (backup != 0u) {
		leaf[2] = 'B';
		leaf[3] = 'A';
		leaf[4] = 'K';
	}
	for (attempt = 0u; attempt < 10u; ++attempt) {
		leaf[5] = (char)('0' + attempt);
		if (sibling_path(target, leaf, path) != KE_FILE_OK)
			return KE_FILE_PATH;
		if (backup != 0u) {
			if (ke_fs_stat(path, &info) < 0) return KE_FILE_OK;
		} else {
			*handle = ke_fs_open(path, KE_FS_WRITE | KE_FS_CREATE |
					     KE_FS_EXCL);
			if (*handle >= 0) return KE_FILE_OK;
		}
	}
	return KE_FILE_PATH;
}

int ke_file_load(struct ke_store *store, const char *path)
{
	struct ke_normalizer normalizer;
	int handle;
	int result = KE_FILE_OK;
	uint16_t input_at = 0u;
	uint16_t input_count = 0u;

	normalizer.pending_cr = 0u;
	normalizer.eof = 0u;
	handle = ke_fs_open(path, KE_FS_READ);
	if (handle < 0) return handle;
	for (;;) {
		uint16_t consumed;
		uint16_t produced;

		if (input_at == input_count) {
			int count = ke_fs_read((uint16_t)handle, input_buffer,
					       sizeof(input_buffer));
			if (count < 0) { result = count; break; }
			if (count == 0) {
				if (ke_normalize_finish(&normalizer, output_buffer,
						       sizeof(output_buffer),
						       &produced) < 0) {
					result = KE_FILE_BINARY;
					break;
				}
				if (produced != 0u &&
				    ke_store_insert(store, ke_store_length(store),
						    output_buffer, produced) !=
					KE_STORE_OK) result = KE_FILE_TOO_LARGE;
				break;
			}
			input_at = 0u;
			input_count = (uint16_t)count;
		}
		if (ke_normalize(&normalizer, input_buffer + input_at,
				 (uint16_t)(input_count - input_at), output_buffer,
				 sizeof(output_buffer), &consumed, &produced) < 0) {
			result = KE_FILE_BINARY;
			break;
		}
		input_at = (uint16_t)(input_at + consumed);
		if (produced != 0u &&
		    ke_store_insert(store, ke_store_length(store), output_buffer,
				    produced) != KE_STORE_OK) {
			result = KE_FILE_TOO_LARGE;
			break;
		}
		if (normalizer.eof != 0u) break;
	}
	(void)ke_fs_close((uint16_t)handle);
	return result;
}

static int write_all(int handle, const uint8_t *buffer, uint16_t length)
{
	uint16_t done = 0u;

	while (done < length) {
		int result = ke_fs_write((uint16_t)handle, buffer + done,
					 (uint16_t)(length - done));

		if (result <= 0) return result < 0 ? result : -KOS_EIO;
		done = (uint16_t)(done + result);
	}
	return KE_FILE_OK;
}

static void copy_path(char *target, uint16_t capacity, const char *source)
{
	uint16_t length = text_length(source, capacity);
	uint16_t i;

	if (capacity == 0u) return;
	if (length >= capacity) length = (uint16_t)(capacity - 1u);
	for (i = 0u; i < length; ++i) target[i] = source[i];
	target[length] = '\0';
}

int ke_file_save(struct ke_store *store, const char *path,
		 char *recovery, uint16_t recovery_size)
{
	struct ke_file_info existing;
	ke_pos_t at = 0ul;
	int handle = -1;
	int result;
	uint8_t had_existing;

	if (recovery != (char *)0 && recovery_size != 0u) recovery[0] = '\0';
	result = unique_path(path, temporary_path, 0u, &handle);
	if (result != KE_FILE_OK) return result;
	while (at < ke_store_length(store)) {
		uint16_t chunk = ke_store_length(store) - at > KE_IO_SIZE ?
				 KE_IO_SIZE :
				 (uint16_t)(ke_store_length(store) - at);

		if (ke_store_read(store, at, output_buffer, chunk) != KE_STORE_OK) {
			result = -KOS_EIO;
			goto fail_temp;
		}
		result = write_all(handle, output_buffer, chunk);
		if (result < 0) goto fail_temp;
		at += chunk;
	}
	result = ke_fs_sync((uint16_t)handle);
	if (result < 0) goto fail_temp;
	result = ke_fs_close((uint16_t)handle);
	handle = -1;
	if (result < 0) goto fail_temp;
	had_existing = (uint8_t)(ke_fs_stat(path, &existing) >= 0);
	if (had_existing != 0u) {
		result = unique_path(path, backup_path, 1u, &handle);
		if (result != KE_FILE_OK) goto fail_temp;
		result = ke_fs_rename(path, backup_path);
		if (result < 0) goto fail_temp;
	}
	result = ke_fs_rename(temporary_path, path);
	if (result < 0) {
		if (had_existing != 0u && ke_fs_rename(backup_path, path) < 0 &&
		    recovery != (char *)0)
			copy_path(recovery, recovery_size, backup_path);
		goto fail_temp;
	}
	if (had_existing != 0u && ke_fs_unlink(backup_path) < 0) {
		if (recovery != (char *)0)
			copy_path(recovery, recovery_size, backup_path);
	}
	return KE_FILE_OK;

fail_temp:
	if (handle >= 0) (void)ke_fs_close((uint16_t)handle);
	if (ke_fs_unlink(temporary_path) < 0 && recovery != (char *)0)
		copy_path(recovery, recovery_size, temporary_path);
	return result;
}
