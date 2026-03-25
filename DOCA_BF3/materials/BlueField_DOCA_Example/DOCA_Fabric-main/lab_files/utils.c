// Include required header
#include "utils.h"

// Reads a file into memory and returns its size
doca_error_t read_file(char const *path, char **out_bytes, 
	size_t *out_bytes_len) {

    FILE *file;
	char *bytes;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

    long const nb_file_bytes = ftell(file);

	if (nb_file_bytes == -1) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	if (nb_file_bytes == 0) {
		fclose(file);
		return DOCA_ERROR_INVALID_VALUE;
	}

    posix_memalign((void **)&bytes, 64, nb_file_bytes);
	if (bytes == NULL) {
		fclose(file);
		return DOCA_ERROR_NO_MEMORY;
	}

	if (fseek(file, 0, SEEK_SET) != 0) {
		free(bytes);
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

    size_t const read_byte_count = fread(bytes, 1, nb_file_bytes, file);

	fclose(file);

	if (read_byte_count != (size_t)nb_file_bytes) {
		free(bytes);
		return DOCA_ERROR_IO_FAILED;
	}

	*out_bytes = bytes;
	*out_bytes_len = read_byte_count;

	return DOCA_SUCCESS;
}

