#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "cleanup_windows.h"
#include "evidence_windows.h"
#include "shim_windows.h"

struct item {
    unsigned root;
    wchar_t relative[MAX_PATH];
    struct brw_evidence evidence;
    DWORD directory;
};

static bool plain_directory(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* Resolve only components beneath the recorded private root, stopping at links. */
static bool item_path(const wchar_t *root, const wchar_t *relative, wchar_t path[BRW_PATH_CAP])
{
    if (wcslen(root) + wcslen(relative) + 2 > BRW_PATH_CAP)
        return false;
    wcscpy(path, root);
    while (*relative) {
        size_t len = wcscspn(relative, L"\\");
        if (!len || (len == 1 && *relative == L'.') || (len == 2 && !wcsncmp(relative, L"..", 2)) ||
            wcscspn(relative, L"/:*?\"<>|") < len || relative[len-1] == L'.' || relative[len-1] == L' ')
            return false;
        DWORD attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES && !plain_directory(path))
            return false;
        wcscat(path, L"\\");
        wcsncat(path, relative, len);
        relative += len;
        if (*relative) ++relative;
    }
    return true;
}

static bool record_item(FILE *file, const wchar_t *root, struct item *item, unsigned depth)
{
    wchar_t path[BRW_PATH_CAP];
    if (depth > 17 || !item_path(root, item->relative, path))
        return false;
    bool folder = plain_directory(path);
    if (folder) {
        size_t len = wcslen(item->relative);
        WIN32_FIND_DATAW child;
        if (wcslen(path) + 3 > BRW_PATH_CAP) return false;
        wcscat(path, L"\\*");
        HANDLE search = FindFirstFileW(path, &child);
        if (search == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        if (search != INVALID_HANDLE_VALUE) {
            bool ok = true;
            do {
                if (!wcscmp(child.cFileName, L".") || !wcscmp(child.cFileName, L"..")) continue;
                if (len + wcslen(child.cFileName) + 2 > MAX_PATH) { ok = false; break; }
                if (len) item->relative[len] = L'\\';
                wcscpy(item->relative + len + !!len, child.cFileName);
                ok = record_item(file, root, item, depth + 1);
                item->relative[len] = 0;
                if (!ok) break;
            } while (FindNextFileW(search, &child));
            ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
            FindClose(search);
            if (!ok) return false;
        }
        path[wcslen(path) - 2] = 0;
    }
    item->directory = folder;
    return brw_node_evidence(path, &item->evidence) && fwrite(item, sizeof *item, 1, file) == 1;
}

bool brw_record_cleanup(FILE *file, const wchar_t *roots[], unsigned count)
{
    struct item item = {0};
    for (unsigned i = 0; i < count; ++i) {
        item.root = i;
        if (!record_item(file, roots[i], &item, 0)) return false;
    }
    item.root = count;
    return fwrite(&item, sizeof item, 1, file) == 1;
}

static const char *check_items(FILE *file, const wchar_t *roots[], unsigned count, bool remove)
{
    struct item item;
    static const struct brw_evidence missing = {{0}};
    while (fread(&item, sizeof item, 1, file) == 1) {
        if (item.root == count)
            return fgetc(file) == EOF && !ferror(file) ? NULL : "publication-journal";
        wchar_t path[BRW_PATH_CAP];
        if (item.root > count || item.relative[MAX_PATH-1] || item.directory > 1 ||
            !item_path(roots[item.root], item.relative, path))
            return "publication-journal";
        struct brw_evidence current;
        if (!brw_node_evidence(path, &current)) return "publication-conflict";
        if (!memcmp(&current, &missing, sizeof current)) continue;
        if (memcmp(&current, &item.evidence, sizeof current)) return "publication-conflict";
        if (remove && !(item.directory ? RemoveDirectoryW(path) : DeleteFileW(path)))
            return "publication-cleanup";
    }
    return "publication-journal";
}

const char *brw_run_cleanup(FILE *file, const wchar_t *roots[], unsigned count, bool preview)
{
    long offset = ftell(file);
    if (offset < 0) return "publication-journal";
    const char *error = check_items(file, roots, count, false);
    if (error || preview) return error;
    if (fseek(file, offset, SEEK_SET)) return "publication-journal";
    return check_items(file, roots, count, true);
}
