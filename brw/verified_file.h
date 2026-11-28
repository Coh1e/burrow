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
/* Accept an HTTP(S) transfer URL with an explicit host and no credentials or fragment. */
bool brw_valid_http_url(const char *url);
/* Numeric IPv6 address without brackets. Does not perform a host lookup. */
bool brw_valid_ipv6_literal(const char *address);
bool brw_valid_sha256(const char *hex);
/* Compare a finalized 32-byte SHA-256 with its hexadecimal representation. */
bool brw_sha256_digest_matches(const unsigned char digest[32], const char *hex);
/* Compare a bounded byte snapshot with a NUL-terminated SHA-256 hex digest. */
bool brw_sha256_matches(const unsigned char *data, size_t size, const char *hex);

#endif
