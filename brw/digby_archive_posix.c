#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "digby_archive_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

struct digby_archive_stage {
    int parent, root;
    char *path, *name;
    char temporary[64];
};

static const int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

static int child_directory(int parent, const char *name, bool create)
{
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    return openat(parent, name, directory_flags);
}

/* Consume the starting fd. The returned leaf borrows from the split path. */
static int walk_parent(int fd, char *path, bool create, char **leaf)
{
    char *slash;
    while (fd >= 0 && (slash = strchr(path, '/')) != NULL) {
        *slash = 0;
        int next = child_directory(fd, path, create);
        close(fd);
        fd = next;
        path = slash + 1;
    }
    *leaf = path;
    return fd;
}

static const char *open_destination(struct digby_archive_stage *s,
                                    const char *workspace, const char *relative)
{
    s->parent = -1;
    s->path = strdup(relative);
    if (!s->path) return "out-of-memory";
    s->parent = walk_parent(open(workspace, directory_flags), s->path, false, &s->name);
    if (s->parent < 0) return "unsafe-parent";
    struct stat info;
    if (!fstatat(s->parent, s->name, &info, AT_SYMLINK_NOFOLLOW)) return "destination-exists";
    return errno == ENOENT ? NULL : "inspect-output";
}

static void close_destination(struct digby_archive_stage *s)
{
    if (s->parent >= 0) close(s->parent);
    free(s->path);
}

const char *digby_archive_destination(const char *workspace, const char *relative)
{
    struct digby_archive_stage stage;
    const char *error = open_destination(&stage, workspace, relative);
    close_destination(&stage);
    return error;
}

const char *digby_archive_begin(const char *workspace, const char *relative,
                                struct digby_archive_stage **out)
{
    *out = NULL;
    struct digby_archive_stage *s = malloc(sizeof *s);
    if (!s) return "out-of-memory";
    const char *error = open_destination(s, workspace, relative);
    if (!error) {
        error = "create-stage";
        for (unsigned i = 0; i < 128; ++i) {
            snprintf(s->temporary, sizeof s->temporary, ".brw-unpack-%ld-%u", (long)getpid(), i);
            if (!strcasecmp(s->name, s->temporary)) continue;
            if (mkdirat(s->parent, s->temporary, 0700)) {
                if (errno == EEXIST) continue;
                break;
            }
            s->root = openat(s->parent, s->temporary, directory_flags);
            if (s->root >= 0) {
                *out = s;
                return NULL;
            }
            if (unlinkat(s->parent, s->temporary, AT_REMOVEDIR)) error = "cleanup-stage";
            break;
        }
    }
    close_destination(s);
    free(s);
    return error;
}

const char *digby_archive_entry(struct digby_archive_stage *s, const char *relative,
                                bool directory, bool executable, FILE **out)
{
    *out = NULL;
    char *path = strdup(relative), *name;
    if (!path) return "out-of-memory";
    int parent = walk_parent(dup(s->root), path, true, &name);
    const char *error = "archive-entry";
    if (parent >= 0) {
        if (directory) {
            int fd = child_directory(parent, name, true);
            if (fd >= 0) { close(fd); error = NULL; }
        } else {
            int fd = openat(parent, name, O_WRONLY | O_NOFOLLOW | O_CREAT | O_EXCL | O_CLOEXEC,
                            executable ? 0700 : 0600);
            if (fd >= 0) {
                *out = fdopen(fd, "wb");
                if (*out) error = NULL;
                else close(fd);
            }
        }
        close(parent);
    }
    free(path);
    return error;
}

/* Consume fd and remove entries underneath it without following links. */
static bool empty_directory(int fd)
{
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return false; }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(dir);
        if (!item) { if (errno) ok = false; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat info;
        if (fstatat(fd, item->d_name, &info, AT_SYMLINK_NOFOLLOW)) { ok = false; continue; }
        int flags = 0;
        if (S_ISDIR(info.st_mode)) {
            int child = openat(fd, item->d_name, directory_flags);
            if (child < 0 || !empty_directory(child)) { ok = false; continue; }
            flags = AT_REMOVEDIR;
        }
        if (unlinkat(fd, item->d_name, flags)) ok = false;
    }
    if (closedir(dir)) ok = false;
    return ok;
}

static bool stage_owned(const struct digby_archive_stage *s)
{
    struct stat named, opened;
    return !fstat(s->root, &opened) &&
           !fstatat(s->parent, s->temporary, &named, AT_SYMLINK_NOFOLLOW) &&
           named.st_dev == opened.st_dev && named.st_ino == opened.st_ino;
}

const char *digby_archive_finish(struct digby_archive_stage *s, bool publish)
{
    const char *error = NULL;
    if (!stage_owned(s)) error = "stage-changed";
    else if (publish) {
#ifdef __APPLE__
        int status = renameatx_np(s->parent, s->temporary, s->parent, s->name, RENAME_EXCL);
#else
        int status = renameat2(s->parent, s->temporary, s->parent, s->name, RENAME_NOREPLACE);
#endif
        if (status) error = errno == EEXIST || errno == ENOTEMPTY ? "destination-exists" : "publish-directory";
    }
    if (!publish || error) {
        int fd = dup(s->root);
        if (fd < 0 || !empty_directory(fd) || !stage_owned(s) ||
            unlinkat(s->parent, s->temporary, AT_REMOVEDIR)) error = "cleanup-stage";
    }
    close(s->root);
    close_destination(s);
    free(s);
    return error;
}
