// Include required headers
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h> 
#include <doca_types.h>
#include <doca_buf.h>
#include <doca_log.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_compress.h>

#include "definitions.h"
#include "callbacks.h"
#include "utils.h"

DOCA_LOG_REGISTER(LAB5::LOGIC);

// Define the main logic function
doca_error_t run(struct program_state *state) {
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};

    DOCA_LOG_INFO("Initializing program state");
    memset(state, 0, sizeof(*state));

    DOCA_LOG_INFO("Allocating buffer with size of %d", BUFFER_SIZE);
    state->base.buffer_size = BUFFER_SIZE;
    state->base.buffer = (uint8_t *)malloc(state->base.buffer_size);
	state->base.buf_inventory_size = BUF_INVENTORY_SIZE;

    if (state->base.buffer == NULL)
		return DOCA_ERROR_NO_MEMORY;

	state->base.available_buffer = state->base.buffer;

    struct doca_devinfo **dev_list;
    uint32_t nb_devs;
	int res = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", 
			doca_error_get_descr(res));
		return res;
	}
	for (int i = 0; i < nb_devs; i++) {
        res = doca_compress_cap_task_decompress_deflate_is_supported(dev_list[i]);
        if (res != DOCA_SUCCESS) {
            continue;
        }
        if(doca_dev_open(dev_list[i], &state->base.device) == DOCA_SUCCESS) {
            doca_devinfo_destroy_list(dev_list);   
            break;     
        }
    }

	DOCA_LOG_INFO("Creating MMAP");
    EXIT_ON_FAILURE(doca_mmap_create(&state->base.src_mmap));
	EXIT_ON_FAILURE(doca_mmap_add_dev(state->base.src_mmap, state->base.device));
	EXIT_ON_FAILURE(doca_mmap_create(&state->base.dst_mmap));
	EXIT_ON_FAILURE(doca_mmap_add_dev(state->base.dst_mmap, state->base.device));

	DOCA_LOG_INFO("Creating buf inventory");
	EXIT_ON_FAILURE(doca_buf_inventory_create(state->base.buf_inventory_size, 
		&state->base.inventory));
	EXIT_ON_FAILURE(doca_buf_inventory_start(state->base.inventory));

    DOCA_LOG_INFO("Creating PE");
	EXIT_ON_FAILURE(doca_pe_create(&state->base.pe));

    DOCA_LOG_INFO("Creating Compress");
	EXIT_ON_FAILURE(doca_compress_create(state->base.device, &state->compress));
	state->compress_ctx = doca_compress_as_ctx(state->compress);

	EXIT_ON_FAILURE(doca_pe_connect_ctx(state->base.pe, state->compress_ctx));
	
	EXIT_ON_FAILURE(doca_ctx_set_state_changed_cb(state->compress_ctx, 
		compress_state_changed_callback));

	doca_error_t result = doca_compress_task_decompress_deflate_set_conf(
		state->compress,
		decompress_deflate_completed_callback,
		decompress_deflate_error_callback,
		1);

    union doca_data ctx_user_data = {0};
	ctx_user_data.ptr = state;
	doca_ctx_set_user_data(state->compress_ctx, ctx_user_data);

	uint64_t max_buf_size;
	result = doca_compress_cap_task_decompress_deflate_get_max_buf_size(
		doca_dev_as_devinfo(state->base.device),&max_buf_size);
	
	result = doca_ctx_start(state->compress_ctx);

	char *dst_buffer = calloc(1, max_buf_size);
	if (posix_memalign((void **)&dst_buffer, 64, max_buf_size) != 0) {
		DOCA_LOG_ERR("Failed to allocate aligned memory for destination");
		return DOCA_ERROR_NO_MEMORY;
	}
	memset(dst_buffer, 0, max_buf_size);

	result = doca_mmap_set_memrange(state->base.dst_mmap, dst_buffer, 
		max_buf_size);
	result = doca_mmap_start(state->base.dst_mmap);

	char *file_data = NULL;
	size_t file_size;
	result = read_file("infile", &file_data, &file_size);

	result = doca_mmap_set_memrange(state->base.src_mmap, file_data, file_size);
	result = doca_mmap_start(state->base.src_mmap);

	struct doca_buf *src_doca_buf;
	result =
		doca_buf_inventory_buf_get_by_addr(state->base.inventory, 
			state->base.src_mmap, file_data, file_size, &src_doca_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to acquire DOCA buffer representing source buffer: %s",
			     doca_error_get_descr(result));
	}

	struct doca_buf *dst_doca_buf;
	result = doca_buf_inventory_buf_get_by_addr(state->base.inventory,
							state->base.dst_mmap,
						    dst_buffer,
						    max_buf_size,
						    &dst_doca_buf);

	result = doca_buf_set_data(src_doca_buf, file_data, file_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get data length in the DOCA buffer"
			 	"representing source buffer: %s",
					doca_error_get_descr(result));
	}

    union doca_data task_user_data = {0};
	struct compress_deflate_result task_result = {0};
	task_user_data.ptr = &task_result;
	struct doca_compress_task_decompress_deflate *decompress_task;

	result = doca_compress_task_decompress_deflate_alloc_init(state->compress,
		src_doca_buf,
		dst_doca_buf,
		task_user_data,
		&decompress_task);

	struct doca_task *task = doca_compress_task_decompress_deflate_as_task(
		decompress_task);
	state->num_remaining_tasks++;

	result = doca_task_submit(task);
	
	state->run_pe_progress = true;

    while (state->run_pe_progress) {
		if (doca_pe_progress(state->base.pe) == 0) {
			nanosleep(&ts, &ts);
		}
	}

	FILE *out_file = fopen("decompressed", "w");
	if (out_file == NULL) {
		DOCA_LOG_ERR("Unable to open output file: decompressed");
		return DOCA_ERROR_NO_MEMORY;
	}

    size_t data_len;
	result = doca_buf_get_data_len(dst_doca_buf, &data_len);
	fwrite(dst_buffer, sizeof(uint8_t), data_len, out_file);

	fclose(out_file);

	doca_buf_dec_refcount(dst_doca_buf, NULL);
	doca_buf_dec_refcount(src_doca_buf, NULL);
	free(dst_buffer);

	return DOCA_SUCCESS;
}

// Define the run_lab_logic function
doca_error_t run_lab_logic(void) {
	struct program_state state;
	doca_error_t status = run(&state);

	return status;
}

