#define WIN32_LEAN_AND_MEAN
#include "digby_archive_io.h"
#include "blueprint_files_windows.h"
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <wchar.h>

struct created_member {
    struct created_member *next;
    BY_HANDLE_FILE_INFORMATION id;
    wchar_t path[];
};

struct digby_archive_stage {
    HANDLE root, parent;
    wchar_t *name;
    struct created_member *created;
};

/* On success the caller owns both the pinned parent and the leaf name. */
static const char *destination(const char *workspace, const char *relative, HANDLE *parent,
                               wchar_t **name)
{
    wchar_t *path = wide(relative), *leaf;
    if (!path) return "out-of-memory";
    HANDLE dir = open_parent(workspace, path, &leaf);
    const char *error = "unsafe-parent";
    if (dir != INVALID_HANDLE_VALUE) {
        HANDLE found = open_child(dir, leaf, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, 0);
        if (found != INVALID_HANDLE_VALUE) {
            CloseHandle(found);
            error = "destination-exists";
        } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            memmove(path, leaf, (wcslen(leaf) + 1) * sizeof *path);
            *name = path;
            *parent = dir;
            return NULL;
        } else error = "inspect-output";
        CloseHandle(dir);
    }
    free(path);
    return error;
}

const char *digby_archive_destination(const char *workspace, const char *relative)
{
    HANDLE parent;
    wchar_t *name;
    const char *error = destination(workspace, relative, &parent, &name);
    if (!error) {
        CloseHandle(parent);
        free(name);
    }
    return error;
}

const char *digby_archive_begin(const char *workspace, const char *relative,
                                struct digby_archive_stage **out)
{
    *out = NULL;
    struct digby_archive_stage *stage = calloc(1, sizeof *stage);
    if (!stage) return "out-of-memory";
    const char *error = destination(workspace, relative, &stage->parent, &stage->name);
    if (!error) {
        for (unsigned i = 0; i < 8; ++i) {
            wchar_t name[48];
            ULONGLONG random;
            if (BCryptGenRandom(NULL, (PUCHAR)&random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ||
                swprintf(name, 48, L".brw-unpack-%016llx", random) < 0) break;
            if (!_wcsicmp(name, stage->name)) continue;
            stage->root = open_child(stage->parent, name, FILE_TRAVERSE | FILE_READ_ATTRIBUTES | DELETE,
                0, FILE_CREATE, FILE_DIRECTORY_FILE);
            if (stage->root != INVALID_HANDLE_VALUE) {
                *out = stage;
                return NULL;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        CloseHandle(stage->parent);
        free(stage->name);
        error = "create-stage";
    }
    free(stage);
    return error;
}

static bool discard(HANDLE h)
{
    FILE_DISPOSITION_INFO remove = {TRUE};
    return SetFileInformationByHandle(h, FileDispositionInfo, &remove, sizeof remove) != 0;
}

static const char *open_member(struct digby_archive_stage *stage, HANDLE parent,
                               const wchar_t *relative, const wchar_t *name, bool directory, HANDLE *out)
{
    ULONG options = directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE;
    ACCESS_MASK access = FILE_READ_ATTRIBUTES | (directory ? FILE_TRAVERSE : GENERIC_WRITE);
    if (directory) {
        HANDLE dir = open_child(parent, name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, options);
        if (dir != INVALID_HANDLE_VALUE) {
            if (ordinary(dir, true)) {
                *out = dir;
                return NULL;
            }
            CloseHandle(dir);
            return "archive-entry";
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return "archive-entry";
    }
    size_t bytes = (wcslen(relative) + 1) * sizeof *relative;
    struct created_member *member = malloc(sizeof *member + bytes);
    if (!member) return "out-of-memory";
    HANDLE h = open_child(parent, name, access | DELETE, 0, FILE_CREATE, options);
    if (h == INVALID_HANDLE_VALUE) {
        free(member);
        return "archive-entry";
    }
    if (!GetFileInformationByHandle(h, &member->id)) {
        const char *error = discard(h) ? "archive-entry" : "cleanup-stage";
        CloseHandle(h);
        free(member);
        return error;
    }
    memcpy(member->path, relative, bytes);
    member->next = stage->created;
    stage->created = member;
    *out = h;
    return NULL;
}

const char *digby_archive_entry(struct digby_archive_stage *stage, const char *relative,
                                bool directory, bool executable, FILE **out)
{
    (void)executable;
    *out = NULL;
    wchar_t *path = wide(relative);
    if (!path) return "out-of-memory";
    HANDLE parent = stage->root;
    const char *error = NULL;
    for (wchar_t *part = path;;) {
        wchar_t *slash = wcschr(part, L'/');
        if (slash) *slash = 0;
        HANDLE h;
        error = open_member(stage, parent, path, part, slash || directory, &h);
        if (parent != stage->root) CloseHandle(parent);
        if (error) break;
        if (!slash) {
            if (directory) CloseHandle(h);
            else {
                int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
                if (fd < 0) CloseHandle(h);
                else {
                    *out = _fdopen(fd, "wb");
                    if (!*out) _close(fd);
                }
                if (!*out) error = "write-stage";
            }
            break;
        }
        *slash = L'/';
        part = slash + 1;
        parent = h;
    }
    free(path);
    return error;
}

/* Windows requires children to be closed before their directory can be renamed.
   Rollback reopens only objects we created, checking their identities by handle. */
static bool remove_member(HANDLE root, struct created_member *member)
{
    HANDLE parent;
    if (!DuplicateHandle(GetCurrentProcess(), root, GetCurrentProcess(), &parent,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    wchar_t *leaf;
    parent = descend_parent(parent, member->path, &leaf);
    if (parent == INVALID_HANDLE_VALUE) return false;
    HANDLE h = open_child(parent, leaf, DELETE | FILE_READ_ATTRIBUTES, 0, FILE_OPEN, 0);
    CloseHandle(parent);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    bool removed = GetFileInformationByHandle(h, &info) &&
        info.dwVolumeSerialNumber == member->id.dwVolumeSerialNumber &&
        info.nFileIndexHigh == member->id.nFileIndexHigh && info.nFileIndexLow == member->id.nFileIndexLow &&
        CompareFileTime(&info.ftCreationTime, &member->id.ftCreationTime) == 0 && discard(h);
    CloseHandle(h);
    return removed;
}

const char *digby_archive_finish(struct digby_archive_stage *stage, bool publish_output)
{
    const char *error = NULL;
    if (publish_output) {
        DWORD status = rename_sibling(stage->root, stage->name, false);
        if (status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) error = "destination-exists";
        else if (status) error = status == ERROR_NOT_ENOUGH_MEMORY ? "out-of-memory" : "publish-directory";
    }
    bool rollback = !publish_output || error;
    while (stage->created) {
        struct created_member *member = stage->created;
        stage->created = member->next;
        if (rollback && !remove_member(stage->root, member)) error = "cleanup-stage";
        free(member);
    }
    if (rollback && !discard(stage->root)) error = "cleanup-stage";
    CloseHandle(stage->root);
    CloseHandle(stage->parent);
    free(stage->name);
    free(stage);
    return error;
}
