#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "init_windows.h"
#include "paths_windows.h"

enum { ENV_CAP = 32767 };

static bool same_path(const wchar_t *start, size_t length, const wchar_t *bin, bool expand)
{
    while (length && (*start == L' ' || *start == L'\t')) {
        ++start;
        --length;
    }
    while (length && (start[length - 1] == L' ' || start[length - 1] == L'\t'))
        --length;
    if (length >= 2 && *start == L'"' && start[length - 1] == L'"') {
        ++start;
        length -= 2;
    }
    if (!length || length >= BRW_PATH_CAP)
        return false;
    wchar_t token[BRW_PATH_CAP], full[BRW_PATH_CAP], expanded[BRW_PATH_CAP];
    wmemcpy(token, start, length);
    token[length] = 0;
    const wchar_t *input = token;
    DWORD size = (DWORD)(length + 1);
    if (expand) {
        size = ExpandEnvironmentStringsW(token, expanded, BRW_PATH_CAP);
        input = expanded;
    }
    if (!size || size >= BRW_PATH_CAP ||
        !((size > 3 && input[1] == L':' && (input[2] == L'\\' || input[2] == L'/')) ||
          (input[0] == L'\\' && input[1] == L'\\')))
        return false;
    size = GetFullPathNameW(input, BRW_PATH_CAP, full, NULL);
    if (!size || size >= BRW_PATH_CAP)
        return false;
    while (size > 3 && full[size - 1] == L'\\')
        full[--size] = 0;
    return !_wcsicmp(full, bin);
}

const char *brw_append_user_path(wchar_t *value, size_t capacity, DWORD type,
                                 const wchar_t *bin, bool apply, bool *changed)
{
    *changed = false;
    if (wcspbrk(bin, L";\""))
        return "invalid-path-entry";
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return "user-path-read";
    const wchar_t *entry = value;
    while (*entry) {
        const wchar_t *end = entry;
        bool quoted = false;
        while (*end && (*end != L';' || quoted)) {
            if (*end == L'"')
                quoted = !quoted;
            ++end;
        }
        if (quoted)
            return "user-path-read";
        if (same_path(entry, (size_t)(end - entry), bin, type == REG_EXPAND_SZ))
            return NULL;
        entry = *end ? end + 1 : end;
    }
    if (type == REG_EXPAND_SZ && wcschr(bin, L'%'))
        return "invalid-path-entry";
    size_t length = wcslen(value);
    bool separator = length && value[length - 1] != L';';
    if (length + separator + wcslen(bin) + 1 > capacity)
        return "user-path-too-long";
    if (apply) {
        if (separator)
            value[length++] = L';';
        wcscpy(value + length, bin);
    }
    *changed = true;
    return NULL;
}

const char *brw_initialize_path(HKEY key, const wchar_t *bin, bool preview, bool *changed)
{
    *changed = false;
    wchar_t value[ENV_CAP] = L"";
    DWORD type = REG_EXPAND_SZ;
    DWORD bytes = sizeof value;
    LSTATUS status = key ? RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)value, &bytes)
                         : ERROR_FILE_NOT_FOUND;
    if (status == ERROR_FILE_NOT_FOUND) {
        type = REG_EXPAND_SZ;
    } else if (status != ERROR_SUCCESS || !bytes || bytes % sizeof *value ||
               value[bytes / sizeof *value - 1] || wcslen(value) + 1 != bytes / sizeof *value) {
        return "user-path-read";
    }
    const char *error = brw_append_user_path(value, ENV_CAP, type, bin, !preview, changed);
    if (!error && *changed && !preview) {
        bytes = (DWORD)((wcslen(value) + 1) * sizeof *value);
        if (RegSetValueExW(key, L"Path", 0, type, (const BYTE *)value, bytes) != ERROR_SUCCESS) {
            *changed = false;
            return "user-path-write";
        }
    }
    return error;
}
