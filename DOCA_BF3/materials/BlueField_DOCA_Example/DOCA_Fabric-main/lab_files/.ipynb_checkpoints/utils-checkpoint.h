// Prevent multiple inclusions of this header
#ifndef UTILS_H
#define UTILS_H

// Include required headers
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_types.h>
#include <doca_dma.h>
#include <doca_pe.h>

// Macro to exit on DOCa error and log the failure
#define EXIT_ON_FAILURE(_expression_) \
{ \
    doca_error_t _status_ = _expression_; \
\
    if (_status_ != DOCA_SUCCESS) { \
        DOCA_LOG_ERR("%s failed with status %s", __func__, \
            doca_error_get_descr(_status_)); \
        return _status_; \
    } \
}

// Function prototype to read file contents into memory
doca_error_t read_file(char const *path, char **out_bytes, 
    size_t *out_bytes_len);

// End of conditional block to avoid multiple header inclusions
#endif



