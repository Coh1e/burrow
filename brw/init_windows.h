#ifndef BRW_INIT_WINDOWS_H
#define BRW_INIT_WINDOWS_H

#include <windows.h>
#include <stdbool.h>

/* Plans an append to a NUL-terminated PATH within capacity wchar_t elements.
   apply controls whether the caller-owned buffer is updated. */
const char *brw_append_user_path(wchar_t *value, size_t capacity, DWORD type,
                                 const wchar_t *bin, bool apply, bool *changed);
/* Borrows an open environment key. Returns a static error or NULL. When
   successful, changed indicates whether the value needs an append. */
const char *brw_initialize_path(HKEY key, const wchar_t *bin, bool preview, bool *changed);
/* Returns an unowned mutex for a short application-controlled purpose string.
   The caller owns the handle. */
HANDLE brw_user_mutex(const wchar_t *purpose);
/* Run initialization against a borrowed registry root. */
int brw_init_with_registry(HKEY root, bool json, bool preview);

#endif
