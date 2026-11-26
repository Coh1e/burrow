#ifndef BRW_SHIM_WINDOWS_H
#define BRW_SHIM_WINDOWS_H

#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

enum { BRW_PATH_CAP = 4096 };
#define BRW_SHIM_SUFFIX L".target"
#define BRW_SHIM_PATH_TAG L"brw-path-v1="

/* Buffer holds the executable path and has BRW_PATH_CAP wchar_t elements. */
static inline int brw_append_shim_suffix(wchar_t *path, size_t length)
{
    if (length + sizeof BRW_SHIM_SUFFIX / sizeof(wchar_t) > BRW_PATH_CAP)
        return 0;
    wcscpy(path + length, BRW_SHIM_SUFFIX);
    return 1;
}

/* Ordinary entries contain only a NUL-terminated UTF-16 target. A locked
   default may append one NUL-terminated dependency PATH to the same record.
   The caller owns the optional prefix. Reject partial wide characters. */
static inline int brw_read_shim_target(FILE *file, wchar_t target[BRW_PATH_CAP], wchar_t **prefix)
{
    *prefix = NULL;
    size_t n = 0;
    do {
        if (n == BRW_PATH_CAP || fread(target + n, sizeof *target, 1, file) != 1) return 0;
    } while (target[n++] != 0);
    if (n < 2) return 0;
    int first = fgetc(file);
    if (first == EOF) return !ferror(file);
    if (ungetc(first, file) == EOF) return 0;
    enum { TAG_LENGTH = sizeof BRW_SHIM_PATH_TAG / sizeof(wchar_t) - 1, RECORD_LIMIT = TAG_LENGTH + 32767 };
    wchar_t *path = malloc(RECORD_LIMIT * sizeof *path);
    if (!path) return 0;
    size_t bytes = fread(path, 1, RECORD_LIMIT * sizeof *path, file);
    n = bytes / sizeof *path;
    if (bytes % sizeof *path || n < TAG_LENGTH + 2 || path[n - 1] || wcslen(path) != n - 1 ||
        wcsncmp(path, BRW_SHIM_PATH_TAG, TAG_LENGTH) ||
        fgetc(file) != EOF || ferror(file)) {
        free(path);
        return 0;
    }
    wmemmove(path, path + TAG_LENGTH, n - TAG_LENGTH);
    *prefix = path;
    return 1;
}

#endif
