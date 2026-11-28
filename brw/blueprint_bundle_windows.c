#define WIN32_LEAN_AND_MEAN
#include "blueprint_bundle_io.h"
#include "blueprint_files_windows.h"

struct bp_directory *bp_directory_open(const char *path)
{
    wchar_t *name = wide(path);
    HANDLE root = name ? open_directory(name) : INVALID_HANDLE_VALUE;
    free(name);
    if (root == INVALID_HANDLE_VALUE) return NULL;
    struct bp_directory *dir = malloc(sizeof *dir);
    if (!dir) {
        CloseHandle(root);
        return NULL;
    }
    dir->handle = root;
    return dir;
}

void bp_directory_close(struct bp_directory *dir)
{
    CloseHandle(dir->handle);
    free(dir);
}

static const char *read_bytes(HANDLE file, size_t limit, unsigned char **out, size_t *size)
{
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) return "bundle-file-unavailable";
    if ((ULONGLONG)file_size.QuadPart > limit) return "bundle-limit";
    DWORD length = (DWORD)file_size.QuadPart;
    unsigned char *bytes = malloc((size_t)length + 1);
    if (!bytes) return "out-of-memory";
    DWORD got;
    if (!ReadFile(file, bytes, length, &got, NULL) || got != length) {
        free(bytes);
        return "bundle-file-unavailable";
    }
    bytes[length] = 0;
    *out = bytes;
    *size = length;
    return NULL;
}

const char *brw_read_local_file(const char *path, size_t limit, unsigned char **out, size_t *size)
{
    wchar_t *name = wide(path);
    HANDLE file = name ? CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
    free(name);
    if (file == INVALID_HANDLE_VALUE) return "archive-unavailable";
    const char *error = ordinary(file, false) ? read_bytes(file, limit, out, size) : "archive-unavailable";
    CloseHandle(file);
    return error;
}

const char *bp_directory_read(const struct bp_directory *dir, const char *path,
                              size_t limit, unsigned char **out, size_t *size)
{
    wchar_t *name = wide(path);
    if (!name) return "out-of-memory";
    const char *error = "bundle-file-unavailable";
    HANDLE parent = dir->handle;
    wchar_t *part = name;
    for (;;) {
        wchar_t *slash = wcschr(part, L'/');
        if (slash) *slash = 0;
        HANDLE child = open_child(parent, part, slash ? FILE_TRAVERSE | FILE_READ_ATTRIBUTES : GENERIC_READ,
                                  FILE_SHARE_READ, FILE_OPEN, slash ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
        if (parent != dir->handle) CloseHandle(parent);
        if (child == INVALID_HANDLE_VALUE) break;
        if (!ordinary(child, slash != NULL)) {
            CloseHandle(child);
            break;
        }
        if (!slash) {
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(child, &info) && info.nNumberOfLinks == 1)
                error = read_bytes(child, limit, out, size);
            CloseHandle(child);
            break;
        }
        parent = child;
        part = slash + 1;
    }
    free(name);
    return error;
}
