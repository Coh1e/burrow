#ifndef BRW_BLUEPRINT_FILES_WINDOWS_H
#define BRW_BLUEPRINT_FILES_WINDOWS_H

#include <windows.h>
#include <winternl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static wchar_t *wide(const char *s)
{
    int n = s ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) : 0;
    wchar_t *w = n ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static bool ordinary(HANDLE h, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &info) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

/* Each lookup is one validated component relative to a directory handle.
   Renaming or replacing an ancestor cannot redirect the next lookup. */
static HANDLE open_child(HANDLE parent, const wchar_t *name, ACCESS_MASK access,
                         ULONG share, ULONG disposition, ULONG options)
{
    size_t chars = wcslen(name);
    if (chars > USHRT_MAX / sizeof *name) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING u = {(USHORT)(chars * sizeof *name), (USHORT)(chars * sizeof *name), (PWSTR)name};
    OBJECT_ATTRIBUTES attr;
    InitializeObjectAttributes(&attr, &u, OBJ_CASE_INSENSITIVE, parent, NULL);
    IO_STATUS_BLOCK io;
    HANDLE h;
    NTSTATUS status = NtCreateFile(&h, access | SYNCHRONIZE, &attr, &io, NULL,
        FILE_ATTRIBUTE_NORMAL, share, disposition,
        options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, NULL, 0);
    if (status >= 0) return h;
    SetLastError(RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
}

static HANDLE open_directory(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE && !ordinary(h, true)) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    return h;
}
#endif
