#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "digby_archive_io.h"

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

struct staged_member {
    struct staged_member *next, *children;
    dev_t device;
    ino_t inode;
    bool directory;
    char name[];
};

struct digby_archive_stage {
    int parent, root;
    char *path, *name;
    char temporary[64];
    struct staged_member *members;
};

static const int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

/* Consume the starting fd. The returned leaf borrows from the split path. */
static int walk_parent(int fd, char *path, char **leaf)
{
    char *slash;
    while (fd >= 0 && (slash = strchr(path, '/')) != NULL) {
        *slash = 0;
        int next = openat(fd, path, directory_flags);
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
    s->parent = walk_parent(open(workspace, directory_flags), s->path, &s->name);
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
    struct digby_archive_stage *s = calloc(1, sizeof *s);
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
            /* Without a handle we cannot identify the directory for rollback. */
            error = "cleanup-stage";
            break;
        }
    }
    close_destination(s);
    free(s);
    return error;
}

static bool same_member(const struct staged_member *member, const struct stat *info)
{
    return member->device == info->st_dev && member->inode == info->st_ino &&
           (member->directory ? S_ISDIR(info->st_mode) : S_ISREG(info->st_mode));
}

/* Only recorded directories may be reopened for additional archive members. */
static const char *open_member(int parent, struct staged_member **siblings, const char *name,
                               bool directory, bool executable, int *out, struct staged_member **record)
{
    struct stat info;
    if (directory) {
        for (struct staged_member *member = *siblings; member; member = member->next) {
            if (strcmp(name, member->name)) continue;
            int fd = openat(parent, name, directory_flags);
            if (fd < 0) return "stage-changed";
            if (fstat(fd, &info) || !same_member(member, &info)) {
                close(fd);
                return "stage-changed";
            }
            *record = member;
            *out = fd;
            return NULL;
        }
    }
    size_t length = strlen(name) + 1;
    struct staged_member *member = malloc(sizeof *member + length);
    if (!member) return "out-of-memory";
    int fd;
    if (directory) {
        if (mkdirat(parent, name, 0700)) { free(member); return "archive-entry"; }
        fd = openat(parent, name, directory_flags);
    } else {
        fd = openat(parent, name, O_WRONLY | O_NOFOLLOW | O_CREAT | O_EXCL | O_CLOEXEC,
                    executable ? 0700 : 0600);
    }
    if (fd < 0 || fstat(fd, &info)) {
        const char *error = directory || fd >= 0 ? "cleanup-stage" : "archive-entry";
        if (fd >= 0) close(fd);
        free(member);
        return error;
    }
    member->device = info.st_dev;
    member->inode = info.st_ino;
    member->directory = directory;
    member->children = NULL;
    memcpy(member->name, name, length);
    member->next = *siblings;
    *siblings = member;
    *record = member;
    *out = fd;
    return NULL;
}

const char *digby_archive_entry(struct digby_archive_stage *s, const char *relative,
                                bool directory, bool executable, FILE **out)
{
    *out = NULL;
    char *path = strdup(relative);
    if (!path) return "out-of-memory";
    int parent = s->root;
    struct staged_member **siblings = &s->members;
    const char *error;
    for (char *name = path;;) {
        char *slash = strchr(name, '/');
        if (slash) *slash = 0;
        struct staged_member *member;
        int fd;
        error = open_member(parent, siblings, name, slash || directory, executable, &fd, &member);
        if (parent != s->root) close(parent);
        if (error) break;
        if (!slash) {
            if (directory) close(fd);
            else if (!(*out = fdopen(fd, "wb"))) { close(fd); error = "archive-entry"; }
            break;
        }
        parent = fd;
        siblings = &member->children;
        name = slash + 1;
    }
    free(path);
    return error;
}

static bool stage_owned(const struct digby_archive_stage *s)
{
    struct stat named, opened;
    return !fstat(s->root, &opened) &&
           !fstatat(s->parent, s->temporary, &named, AT_SYMLINK_NOFOLLOW) &&
           named.st_dev == opened.st_dev && named.st_ino == opened.st_ino;
}

/* A negative parent only releases records. Never enumerate or unlink unrecorded entries. */
static bool release_members(struct staged_member *members, int parent)
{
    bool ok = true;
    while (members) {
        struct staged_member *member = members;
        members = member->next;
        int child = -1;
        struct stat info;
        if (parent >= 0 && member->children) {
            child = openat(parent, member->name, directory_flags);
            if (child >= 0 && (fstat(child, &info) || !same_member(member, &info))) {
                close(child);
                child = -1;
            }
        }
        bool missing_parent = parent >= 0 && member->children && child < 0;
        bool children_removed = release_members(member->children, child) && !missing_parent;
        if (child >= 0) close(child);
        if (!children_removed) ok = false;
        if (parent >= 0 && children_removed &&
            (fstatat(parent, member->name, &info, AT_SYMLINK_NOFOLLOW) || !same_member(member, &info) ||
             unlinkat(parent, member->name, member->directory ? AT_REMOVEDIR : 0))) ok = false;
        free(member);
    }
    return ok;
}

const char *digby_archive_finish(struct digby_archive_stage *s, bool publish)
{
    const char *error = NULL;
    bool owned = stage_owned(s);
    if (!owned) error = "stage-changed";
    else if (publish) {
#ifdef __APPLE__
        int status = renameatx_np(s->parent, s->temporary, s->parent, s->name, RENAME_EXCL);
#else
        int status = renameat2(s->parent, s->temporary, s->parent, s->name, RENAME_NOREPLACE);
#endif
        if (status) error = errno == EEXIST || errno == ENOTEMPTY ? "destination-exists" : "publish-directory";
    }
    bool rollback = owned && (!publish || error);
    if (rollback && !stage_owned(s)) {
        rollback = false;
        error = "stage-changed";
    }
    bool removed = release_members(s->members, rollback ? s->root : -1);
    if (rollback && (!removed || !stage_owned(s) || unlinkat(s->parent, s->temporary, AT_REMOVEDIR)))
        error = "cleanup-stage";
    close(s->root);
    close_destination(s);
    free(s);
    return error;
}
