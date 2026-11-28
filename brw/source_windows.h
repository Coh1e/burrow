#ifndef BRW_SOURCE_WINDOWS_H
#define BRW_SOURCE_WINDOWS_H

#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
#include "digby_download_io.h"

typedef const char *(*brw_catalog_visitor)(const wchar_t *source, const wchar_t *catalog, void *context);
/* Visits registered locations without selecting versions or installing packages. */
const char *brw_visit_catalogs(brw_catalog_visitor visitor, void *context);
int brw_source_command(const wchar_t *verb, size_t count, const wchar_t **args,
                       bool json, bool preview);
void brw_json_string(const wchar_t *value);
/* Destination is caller-owned and positioned at the beginning of an empty file. */
const char *brw_download(const wchar_t *location, FILE *destination);
/* Streams one HTTP(S) response; returns the sink's error or a static transport error. */
const char *brw_download_http(const wchar_t *url, digby_download_write write, void *context,
                              bool (*cancelled)(void));
bool brw_http_location(const wchar_t *location);
bool brw_cancelled(void);

#endif
