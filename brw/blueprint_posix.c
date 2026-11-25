#define _POSIX_C_SOURCE 200809L
#include "blueprint_edit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Anchor all lookups, including publication and cleanup, to open directories. */
static int open_parent(const char *workspace, char *relative, char **leaf)
{
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int parent = open(workspace, flags);
    *leaf = relative;
    char *slash;
    while (parent >= 0 && (slash = strchr(*leaf, '/')) != NULL) {
        *slash = 0;
        int next = openat(parent, *leaf, flags);
        close(parent);
        parent = next;
        *leaf = slash + 1;
    }
    return parent;
}

static const char *read_file(int parent, const char *name, struct stat *before,
                             char **out, size_t *length)
{
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return "open-text";
    const char *error = "invalid-text";
    char *text = NULL;
    if (fstat(fd, before) || !S_ISREG(before->st_mode)) goto done;
    if (before->st_size < 0 || before->st_size > BP_TEXT_LIMIT) {
        error = "text-limit";
        goto done;
    }
    size_t size = (size_t)before->st_size;
    text = malloc(size + 1);
    if (!text) { error = "out-of-memory"; goto done; }
    error = "read-text";
    for (size_t used = 0; used < size;) {
        ssize_t n = read(fd, text + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    text[size] = 0;
    *out = text;
    *length = size;
    text = NULL;
    error = NULL;
done:
    free(text);
    close(fd);
    return error;
}

const char *bp_read_workspace_text(const char *workspace, const char *relative, char **out, size_t *size)
{
    char *path = strdup(relative), *leaf;
    if (!path) return "out-of-memory";
    int parent = open_parent(workspace, path, &leaf);
    struct stat info;
    const char *error = parent < 0 ? "unsafe-parent" : read_file(parent, leaf, &info, out, size);
    if (parent >= 0) close(parent);
    free(path);
    return error;
}

static bool unchanged(int parent, const char *name, const struct stat *before)
{
    struct stat now;
    if (fstatat(parent, name, &now, AT_SYMLINK_NOFOLLOW)) return false;
#ifdef __APPLE__
    struct timespec old_time = before->st_ctimespec, new_time = now.st_ctimespec;
#else
    struct timespec old_time = before->st_ctim, new_time = now.st_ctim;
#endif
    return before->st_dev == now.st_dev && before->st_ino == now.st_ino &&
           old_time.tv_sec == new_time.tv_sec && old_time.tv_nsec == new_time.tv_nsec;
}

static const char *write_file(int parent, const char *name, const struct bp_step *s)
{
    char temp[64];
    int fd = -1;
    for (unsigned i = 0; i < 128; ++i) {
        snprintf(temp, sizeof temp, ".brw-bp-%ld-%u", (long)getpid(), i);
        if (!strcmp(temp, name)) continue;
        fd = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return "create-stage";
    FILE *stage = fdopen(fd, "wb");
    const char *error = NULL;
    struct stat before;
    if (!stage) {
        close(fd);
        error = "open-stage";
    } else {
        if (s->op == BP_REPLACE) {
            char *text = NULL;
            size_t size = 0;
            error = read_file(parent, name, &before, &text, &size);
            if (!error) error = bp_replace_text(stage, s, text, size);
            free(text);
            if (!error && fchmod(fileno(stage), before.st_mode & 0777)) error = "stage-mode";
        } else if (fwrite(s->payload->data, 1, s->payload->size, stage) != s->payload->size)
            error = "write-stage";
        if (fclose(stage) && !error) error = "close-stage";
    }
    if (!error && s->op == BP_REPLACE) {
        if (!unchanged(parent, name, &before)) error = "text-changed";
        else if (!renameat(parent, temp, parent, name)) return NULL;
        else error = "publish-file";
    } else if (!error && linkat(parent, temp, parent, name, 0)) {
        error = errno == EEXIST ? "destination-exists" : "publish-file";
    }
    if (unlinkat(parent, temp, 0) && !error) error = "cleanup-stage";
    return error;
}

static const char *file_op(const char *workspace, const struct bp_step *s, bool apply)
{
    const char *error = bp_validate_file_operands(s);
    if (error) return error;
    char *path = strdup(s->path);
    if (!path) return "out-of-memory";
    char *name;
    int parent = open_parent(workspace, path, &name);
    if (parent < 0) error = "unsafe-parent";
    else {
        struct stat info;
        if (!fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(info.st_mode)) error = "unsafe-output-path";
            else if (s->op == BP_COPY) error = "destination-exists";
        } else if (errno != ENOENT) error = "inspect-output";
        if (apply && !error) error = write_file(parent, name, s);
        close(parent);
    }
    free(path);
    return error;
}

static const char *operation(const char *workspace, const struct bp_step *s, bool apply)
{
    if (!workspace || !*workspace || !s) return "invalid-step";
    switch (s->op) {
    case BP_COPY:
    case BP_REPLACE: return file_op(workspace, s, apply);
    case BP_ENV:
        if (!bp_env_name(s->name) || !s->value) return "invalid-environment";
        return apply && setenv(s->name, s->value, 1) ? "environment-write" : NULL;
    default: return "missing-capability";
    }
}

const char *bp_validate_step(const char *workspace, const struct bp_step *s)
{
    return operation(workspace, s, false);
}

const char *bp_apply_step(const char *workspace, const struct bp_step *s)
{
    return operation(workspace, s, true);
}
