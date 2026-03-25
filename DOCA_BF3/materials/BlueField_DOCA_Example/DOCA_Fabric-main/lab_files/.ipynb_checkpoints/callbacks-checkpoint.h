// Include required headers
#include <stdlib.h>
#include <stdint.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dma.h>
#include <doca_types.h>
#include <doca_log.h>
#include <doca_compress.h>
#include <doca_pe.h>
#include "definitions.h"
#include "utils.h"

// Callback triggered when DMA memcpy task completes successfully
void dma_memcpy_completed_callback(struct doca_dma_task_memcpy *dma_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data);

// Callback triggered when DMA memcpy task encounters an error
void dma_memcpy_error_callback(struct doca_dma_task_memcpy *dma_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data);

// Callback to handle changes in compression context state
void compress_state_changed_callback(const union doca_data user_data,
        struct doca_ctx *ctx,
        enum doca_ctx_states prev_state,
        enum doca_ctx_states next_state);

// Callback invokes on successful completion of a decompression task
void decompress_deflate_completed_callback(
    struct doca_compress_task_decompress_deflate *decompress_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data);

// Call invoked when a decompression task fails
void decompress_deflate_error_callback(
    struct doca_compress_task_decompress_deflate *decompress_task,
        union doca_data task_user_data,
        union doca_data ctx_user_data);    

