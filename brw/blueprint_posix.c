#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "blueprint_edit.h"
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/mount.h>
#else
#include <linux/fs.h>
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#endif

FILE *bp_temp_stream(void)
{
    FILE *file = tmpfile();
    if (file && fcntl(fileno(file), F_SETFD, FD_CLOEXEC) < 0) {
        fclose(file);
        return NULL;
    }
    return file;
}

bool bp_create_output_file(struct bp_output_file *file)
{
    char path[] = "/tmp/brw-output-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
    if (!fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        file->path = strdup(path);
        if (file->path) file->stream = fdopen(fd, "rb");
    }
    if (file->stream) return true;
    close(fd);
    unlink(path);
    free(file->path);
    file->path = NULL;
    return false;
}

bool bp_remove_output_file(struct bp_output_file *file)
{
    bool ok = fclose(file->stream) == 0;
    if (unlink(file->path) && errno != ENOENT) ok = false;
    free(file->path);
    return ok;
}

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

/* Some shared mounts omit case-policy metadata. Look up a second spelling of
   an actual single-link file in this directory without modifying any entry. */
static int observe_directory_case(int dir)
{
    int fd = dup(dir);
    if (fd < 0) return -1;
    DIR *entries = fdopendir(fd);
    if (!entries) { close(fd); return -1; }
    int result = -1;
    struct dirent *entry;
    while (result < 0 && (entry = readdir(entries)) != NULL) {
        size_t i = strcspn(entry->d_name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
        if (!entry->d_name[i]) continue;
        struct stat original, alternate;
        if (fstatat(dir, entry->d_name, &original, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(original.st_mode) || original.st_nlink != 1) continue;
        char *name = strdup(entry->d_name);
        if (!name) break;
        name[i] ^= 0x20; /* toggle one ASCII letter */
        if (!fstatat(dir, name, &alternate, AT_SYMLINK_NOFOLLOW))
            result = alternate.st_dev != original.st_dev || alternate.st_ino != original.st_ino;
        else if (errno == ENOENT) result = 1;
        free(name);
    }
    closedir(entries);
    return result;
}

/* Return 1 for sensitive, 0 for folded, -1 when the mount cannot tell us.
   Preview must not create a temporary entry to probe a directory. */
static int directory_case_sensitive(int dir)
{
#ifdef __APPLE__
    struct statfs volume;
    struct attrlist attrs = { .bitmapcount = ATTR_BIT_MAP_COUNT,
                              .volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES };
    struct { uint32_t size; vol_capabilities_attr_t caps; } reply;
    if (fstatfs(dir, &volume) ||
        getattrlist(volume.f_mntonname, &attrs, &reply, sizeof reply, 0) ||
        reply.size != sizeof reply ||
        !(reply.caps.valid[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_CASE_SENSITIVE))
        return observe_directory_case(dir);
    return !!(reply.caps.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_CASE_SENSITIVE);
#else
    char wsl_flag;
    if (fgetxattr(dir, "system.wsl_case_sensitive", &wsl_flag, sizeof wsl_flag) == 1 &&
        (wsl_flag == '0' || wsl_flag == '1')) return wsl_flag == '1';
    long flags = 0;
    if (!ioctl(dir, FS_IOC_GETFLAGS, &flags)) return !(flags & FS_CASEFOLD_FL);
    /* ramfs uses case-sensitive VFS lookup and has no per-directory case flag. */
    struct statfs fs;
    if (!fstatfs(dir, &fs) && (unsigned long)fs.f_type == RAMFS_MAGIC) return 1;
    return observe_directory_case(dir);
#endif
}

const char *bp_compare_output_case(const char *workspace, const char *a,
                                   const char *b, enum bp_output_match *match)
{
    char *pa = strdup(a), *pb = strdup(b), *la = NULL, *lb = NULL;
    int da = pa ? open_parent(workspace, pa, &la) : -1;
    int db = pb ? open_parent(workspace, pb, &lb) : -1;
    struct stat ia, ib;
    const char *error = "inspect-output";
    if (da >= 0 && db >= 0 && !fstat(da, &ia) && !fstat(db, &ib)) {
        error = NULL;
        bool same_dir = ia.st_dev == ib.st_dev && ia.st_ino == ib.st_ino;
        *match = same_dir ? BP_SAME_OUTPUT : BP_DISTINCT_OUTPUTS;
        if (same_dir && strcmp(la, lb)) {
            int sensitive = directory_case_sensitive(da);
            if (sensitive < 0) *match = BP_UNKNOWN_CASE;
            else if (sensitive) *match = BP_DISTINCT_OUTPUTS;
        }
    }
    if (da >= 0) close(da);
    if (db >= 0) close(db);
    free(pa);
    free(pb);
    return error;
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

const char bp_path_separator = ':';

static int spawn_command(pid_t *child, const struct bp_step *s,
                         const posix_spawn_file_actions_t *actions, char **argv)
{
    if (!s->tool) return posix_spawnp(child, s->program, actions, NULL, argv, environ);
    if (!s->child_path) return posix_spawn(child, s->program, actions, NULL, argv, environ);

    size_t count = 0;
    while (environ[count]) ++count;
    char **env = malloc((count + 2) * sizeof *env);
    char *path = NULL;
    const char *original = getenv("PATH");
    if (!env || asprintf(&path, "PATH=%s%s%s", s->child_path, original ? ":" : "",
                         original ? original : "") < 0) {
        free(env);
        return ENOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; ++i)
        if (strncmp(environ[i], "PATH=", 5)) env[n++] = environ[i];
    env[n++] = path;
    env[n] = NULL;
    int rc = posix_spawn(child, s->program, actions, NULL, argv, env);
    free(path);
    free(env);
    return rc;
}

static const char *launch_command(const struct bp_step *s, const char *workspace, char *script,
                                  unsigned long *exit_code, bool *started)
{
    char *argv[BP_MAX_ARGS + 2] = {s->program};
    for (size_t i = 0; i < s->argc; ++i)
        argv[i + 1] = s->payload && i == s->script_arg ? script : s->args[i];
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) return "child-launch";
    int rc = posix_spawn_file_actions_addchdir_np(&actions, workspace);
    int output = s->capture_stdout ? fileno(s->capture_stdout) : STDERR_FILENO;
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    pid_t child;
    if (!rc) rc = spawn_command(&child, s, &actions, argv);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) return "child-launch";
    if (started) *started = true;
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return "child-wait";
    if (!WIFEXITED(status)) return "child-failed";
    if (exit_code) *exit_code = (unsigned long)WEXITSTATUS(status);
    return WEXITSTATUS(status) && !exit_code ? "child-failed" : NULL;
}

const char *bp_run_command(const char *workspace, const struct bp_step *s, bool apply,
                           unsigned long *exit_code, bool *started)
{
    if (started) *started = false;
    struct stat st;
    if (stat(workspace, &st) || !S_ISDIR(st.st_mode)) return "invalid-workspace";
    if (!apply) return NULL;
    if (!s->payload) return launch_command(s, workspace, NULL, exit_code, started);
    char dir[] = "/tmp/brw-command-XXXXXX", script[BP_DETAIL_CAP];
    if (!mkdtemp(dir)) return "payload-stage";
    const char *base = strrchr(s->payload->path, '/');
    snprintf(script, sizeof script, "%s/%s", dir, base ? base + 1 : s->payload->path);
    FILE *file = fopen(script, "wb");
    const char *error = "payload-stage";
    if (file) {
        bool written = fwrite(s->payload->data, 1, s->payload->size, file) == s->payload->size;
        if (!fclose(file) && written) error = launch_command(s, workspace, script, exit_code, started);
        if (unlink(script) && !error) error = "cleanup-stage";
    }
    if (rmdir(dir) && !error) error = "cleanup-stage";
    return error;
}

static const char *operation(const char *workspace, const struct bp_step *s, bool apply)
{
    if (!workspace || !*workspace || !s) return "invalid-step";
    switch (s->op) {
    case BP_COPY:
    case BP_REPLACE: return file_op(workspace, s, apply);
    case BP_RUN: return bp_run_command(workspace, s, apply, NULL, NULL);
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
