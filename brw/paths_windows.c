#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "paths_windows.h"

static bool absolute_path(const wchar_t *path)
{
    return (wcslen(path) >= 3 && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/')) ||
           ((path[0] == L'\\' || path[0] == L'/') &&
            (path[1] == L'\\' || path[1] == L'/'));
}

/* Start after the volume root, including the host and share of a UNC path. */
static wchar_t *volume_end(wchar_t *path)
{
    if (path[1] == L':')
        return path + 3;
    if ((path[2] == L'?' || path[2] == L'.') && path[3] == L'\\')
        return NULL;
    wchar_t *host = wcschr(path + 2, L'\\');
    if (!host || host == path + 2 || !host[1] || host[1] == L'\\')
        return NULL;
    wchar_t *share = wcschr(host + 1, L'\\');
    return share ? share + 1 : path + wcslen(path);
}

static bool writable_directory(const wchar_t *path, DWORD access)
{
    HANDLE directory = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (directory == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(directory);
    return true;
}

static bool check_directories(wchar_t *path, bool create, bool writable)
{
    wchar_t *first = volume_end(path);
    if (!first)
        return false;
    wchar_t *parent = first;
    for (wchar_t *part = first;; ++part) {
        if (*part && *part != L'\\')
            continue;
        wchar_t saved = *part;
        *part = 0;
        DWORD attrs = GetFileAttributesW(path);
        bool available = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (part != first && (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND)) {
                if (!create) {
                    if (!writable) {
                        *part = saved;
                        return true;
                    }
                    *part = saved;
                    wchar_t separator = *parent;
                    *parent = 0;
                    available = writable_directory(path, FILE_ADD_SUBDIRECTORY);
                    *parent = separator;
                    return available;
                } else {
                    available = CreateDirectoryW(path, NULL) != 0;
                    if (!available && GetLastError() == ERROR_ALREADY_EXISTS) {
                        attrs = GetFileAttributesW(path);
                        available = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
                    }
                }
            } else if (part == first && path[1] != L':') {
                available = GetDriveTypeW(path) == DRIVE_REMOTE;
            }
        }
        if (available && !saved && (create || writable))
            available = writable_directory(path, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
        *part = saved;
        if (!available || !saved)
            return available;
        parent = part;
    }
}

static const char *resolve_path(enum brw_path_kind kind, wchar_t path[BRW_PATH_CAP],
                                bool create, bool writable)
{
    static const struct {
        const wchar_t *variable;
        const wchar_t *fallback;
        const char *error;
    } definitions[] = {
        {L"XDG_CONFIG_HOME", L".config\\burrow", "config-directory"},
        {L"XDG_CACHE_HOME", L".cache\\burrow", "cache-directory"},
        {L"XDG_DATA_HOME", L".local\\share\\burrow", "data-directory"},
        {L"XDG_STATE_HOME", L".local\\state\\burrow", "state-directory"},
        {L"BRW_BIN_DIR", L".local\\bin", "entry-directory"}
    };
    if ((unsigned)kind >= BRW_PATH_COUNT)
        return "invalid-directory";
    enum { ENVIRONMENT_CAP = 32768 };
    wchar_t base[ENVIRONMENT_CAP], full[BRW_PATH_CAP];
    DWORD length = GetEnvironmentVariableW(definitions[kind].variable, base, ENVIRONMENT_CAP);
    if (length >= ENVIRONMENT_CAP)
        return definitions[kind].error;
    const wchar_t *suffix = kind == BRW_BIN ? L"" : L"burrow";
    if (!length || !absolute_path(base)) {
        length = GetEnvironmentVariableW(L"USERPROFILE", base, BRW_PATH_CAP);
        if (!length || length >= BRW_PATH_CAP || !absolute_path(base))
            return definitions[kind].error;
        suffix = definitions[kind].fallback;
    }
    if (wcslen(base) + wcslen(suffix) + 2 > BRW_PATH_CAP)
        return definitions[kind].error;
    if (*suffix)
        swprintf(full, BRW_PATH_CAP, L"%ls\\%ls", base, suffix);
    else
        wcscpy(full, base);
    length = GetFullPathNameW(full, BRW_PATH_CAP, path, NULL);
    if (!length || length >= BRW_PATH_CAP)
        return definitions[kind].error;
    wchar_t *first = volume_end(path);
    if (!first)
        return definitions[kind].error;
    while (path + length > first && path[length - 1] == L'\\')
        path[--length] = 0;

    /* Validate the volume before accepting a missing descendant. */
    wchar_t saved = *first;
    *first = 0;
    DWORD attrs = GetFileAttributesW(path);
    DWORD type = GetDriveTypeW(path);
    *first = saved;
    if ((attrs == INVALID_FILE_ATTRIBUTES && type != DRIVE_REMOTE) ||
        !check_directories(path, create, writable))
        return definitions[kind].error;
    return NULL;
}

const char *brw_path(enum brw_path_kind kind, wchar_t path[BRW_PATH_CAP], bool create)
{
    return resolve_path(kind, path, create, false);
}

const char *brw_writable_path(enum brw_path_kind kind, wchar_t path[BRW_PATH_CAP])
{
    return resolve_path(kind, path, false, true);
}
