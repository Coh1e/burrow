#ifndef BRW_SOURCE_WINDOWS_H
#define BRW_SOURCE_WINDOWS_H

#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

int brw_source_command(const wchar_t *verb, size_t count, const wchar_t **args,
                       bool json, bool preview);
void brw_json_string(const wchar_t *value);
/* Destination is caller-owned and positioned at the beginning of an empty file. */
const char *brw_download(const wchar_t *location, FILE *destination);
bool brw_http_location(const wchar_t *location);
bool brw_cancelled(void);

#endif
