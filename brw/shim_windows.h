#ifndef BRW_SHIM_WINDOWS_H
#define BRW_SHIM_WINDOWS_H

#include <wchar.h>

enum { BRW_PATH_CAP = 4096 };
#define BRW_SHIM_SUFFIX L".target"

/* Buffer holds the executable path and has BRW_PATH_CAP wchar_t elements. */
static inline int brw_append_shim_suffix(wchar_t *path, size_t length)
{
    if (length + sizeof BRW_SHIM_SUFFIX / sizeof(wchar_t) > BRW_PATH_CAP)
        return 0;
    wcscpy(path + length, BRW_SHIM_SUFFIX);
    return 1;
}

#endif
