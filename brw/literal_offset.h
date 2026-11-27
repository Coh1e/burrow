#ifndef BRW_LITERAL_OFFSET_H
#define BRW_LITERAL_OFFSET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format a decimal byte offset in caller-owned storage, including the trailing
   NUL. Return 0 on success or 1 for a NULL or undersized buffer. On failure
   the buffer is unchanged. No allocation or cleanup is required. */
int brw_format_offset(size_t offset, char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
