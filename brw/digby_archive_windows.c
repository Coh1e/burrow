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

struct digby_file_stage {
    HANDLE file, parent;
    wchar_t *name;
    FILE *stream;
};

/* On success the caller owns both the pinned parent and the leaf name. */
static const char *destination(const struct bp_directory *workspace, const char *relative, HANDLE *parent,
                               wchar_t **name)
{
    wchar_t *path = wide(relative), *leaf;
    if (!path) return "out-of-memory";
    HANDLE root;
    if (!DuplicateHandle(GetCurrentProcess(), workspace->handle, GetCurrentProcess(), &root,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        free(path);
        return "unsafe-parent";
    }
    HANDLE dir = descend_parent(root, path, &leaf);
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

const char *digby_check_destination(const struct bp_directory *workspace, const char *relative)
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

static HANDLE create_stage(HANDLE parent, const wchar_t *destination_name, bool directory)
{
    for (unsigned i = 0; i < 8; ++i) {
        wchar_t name[48];
        ULONGLONG random;
        if (BCryptGenRandom(NULL, (PUCHAR)&random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ||
            swprintf(name, 48, directory ? L".brw-unpack-%016llx" : L".brw-pack-~%016llx", random) < 0) break;
        if (!_wcsicmp(name, destination_name)) continue;
        HANDLE h = open_child(parent, name, (directory ? FILE_TRAVERSE : GENERIC_WRITE) | FILE_READ_ATTRIBUTES | DELETE,
            0, FILE_CREATE, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) break;
    }
    return INVALID_HANDLE_VALUE;
}

/* The returned binary stream owns h; on failure the handle is closed here. */
static FILE *output_stream(HANDLE h)
{
    int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    FILE *stream = _fdopen(fd, "wb");
    if (!stream) _close(fd);
    return stream;
}

const char *digby_archive_begin(const struct bp_directory *workspace, const char *relative,
                                struct digby_archive_stage **out)
{
    *out = NULL;
    struct digby_archive_stage *stage = calloc(1, sizeof *stage);
    if (!stage) return "out-of-memory";
    const char *error = destination(workspace, relative, &stage->parent, &stage->name);
    if (!error) {
        stage->root = create_stage(stage->parent, stage->name, true);
        if (stage->root != INVALID_HANDLE_VALUE) {
            *out = stage;
            return NULL;
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
            else if (!(*out = output_stream(h))) error = "write-stage";
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

const char *digby_file_begin(const struct bp_directory *workspace, const char *relative,
                             struct digby_file_stage **out, FILE **stream)
{
    *out = NULL;
    *stream = NULL;
    struct digby_file_stage *s = calloc(1, sizeof *s);
    if (!s) return "out-of-memory";
    const char *error = destination(workspace, relative, &s->parent, &s->name);
    if (!error) {
        s->file = create_stage(s->parent, s->name, false);
        if (s->file != INVALID_HANDLE_VALUE) {
            HANDLE copy;
            if (DuplicateHandle(GetCurrentProcess(), s->file, GetCurrentProcess(), &copy,
                                0, FALSE, DUPLICATE_SAME_ACCESS) && (s->stream = output_stream(copy))) {
                *out = s;
                *stream = s->stream;
                return NULL;
            }
            error = digby_file_finish(s, false, NULL, NULL);
            return error ? error : "open-stage";
        }
        CloseHandle(s->parent);
        free(s->name);
        error = "create-stage";
    }
    free(s);
    return error;
}

const char *digby_file_finish(struct digby_file_stage *s, bool publish,
                              const char *(*before_publish)(void *context), void *context)
{
    const char *error = NULL;
    if (s->stream && fclose(s->stream)) error = "close-stage";
    if (!error && publish && before_publish) error = before_publish(context);
    if (!error && publish) {
        DWORD status = rename_sibling(s->file, s->name, false);
        if (status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) error = "destination-exists";
        else if (status) error = status == ERROR_NOT_ENOUGH_MEMORY ? "out-of-memory" : "publish-file";
    }
    if ((!publish || error) && !discard(s->file)) error = "cleanup-stage";
    CloseHandle(s->file);
    CloseHandle(s->parent);
    free(s->name);
    free(s);
    return error;
}
