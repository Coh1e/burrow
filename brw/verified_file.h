#ifndef BRW_VERIFIED_FILE_H
#define BRW_VERIFIED_FILE_H

#include <stdbool.h>
#include <stddef.h>

enum { BP_PACKAGE_PATH_CAP = 260, BRW_PAYLOAD_LIMIT = 64 * 1024 * 1024 };
/* Compare n bytes using ASCII case folding. Both inputs must have n bytes. */
bool brw_same_ascii(const char *a, const char *b, size_t n);
bool bp_safe_path(const char *path);
bool bp_safe_package_path(const char *path);
bool bp_valid_utf8(const char *text, size_t size);
bool brw_valid_sha256(const char *hex);
/* Compare a bounded byte snapshot with a NUL-terminated SHA-256 hex digest. */
bool brw_sha256_matches(const unsigned char *data, size_t size, const char *hex);

#endif
