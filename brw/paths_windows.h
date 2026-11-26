#ifndef BRW_PATHS_WINDOWS_H
#define BRW_PATHS_WINDOWS_H

#include <stdbool.h>
#include <wchar.h>
#include "shim_windows.h"

enum brw_path_kind { BRW_CONFIG, BRW_CACHE, BRW_DATA, BRW_STATE, BRW_BIN, BRW_PATH_COUNT };
/* Resolves a path into the caller's BRW_PATH_CAP-element buffer. When create is
   true, creates missing directories. Returns NULL or a static diagnostic. */
const char *brw_path(enum brw_path_kind kind, wchar_t path[BRW_PATH_CAP], bool create);
/* Read-only bin resolution after planned environment changes. NULL overrides
   read the corresponding live variable. An empty override is an empty value. */
const char *brw_planned_bin(wchar_t path[BRW_PATH_CAP], const wchar_t *bin, const wchar_t *home);
/* Like brw_path with read-only resolution, plus a check of write access. */
const char *brw_writable_path(enum brw_path_kind kind, wchar_t path[BRW_PATH_CAP]);
int brw_init(bool json, bool preview);

#endif
