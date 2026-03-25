// Prevent multiple inclusions of this header
#ifndef DEFINITIONS_H
#define DEFINITIONS_H

// Include standard library headers for memory and data types
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Define buffer size, inventory and sleep constants
#define BUFFER_SIZE (1024)
#define BUF_INVENTORY_SIZE (2)
#define SLEEP_IN_NANOS (10 * 1000)	       

// Holds base program resources for compression and decompression
struct program_base {
	struct doca_dev *device;
	struct doca_mmap *src_mmap;
	struct doca_mmap *dst_mmap;
	struct doca_buf_inventory *inventory;
	struct doca_pe *pe;

	size_t buffer_size;
	size_t buf_inventory_size;

	uint8_t *buffer;
	uint8_t *available_buffer; 

	uint32_t num_completed_tasks;
};

// Holds the complete program state
struct program_state {
	struct program_base base;
	
	struct doca_compress *compress;
	struct doca_ctx *compress_ctx;
	bool run_pe_progress;		 
	size_t num_remaining_tasks;	    
};

// Result of deflate operation including status and checksums
struct compress_deflate_result {
	doca_error_t status; 
	uint32_t crc_cs;     
	uint32_t adler_cs;   
};

// End of conditional block to avoid multiple header inclusions
#endif

