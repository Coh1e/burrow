#define _XOPEN_SOURCE 700
#include "blueprint_lock.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static bool ensure_dirs(char *path)
{
    for (char *p = path + 1;; ++p) {
        if (*p != '/' && *p) continue;
        char saved = *p;
        *p = 0;
        struct stat st;
        bool ok = !stat(path, &st) ? S_ISDIR(st.st_mode) :
                  errno == ENOENT && (!mkdir(path, 0700) || errno == EEXIST);
        *p = saved;
        if (!ok || !saved) return ok;
    }
}

static char *xdg(const char *variable, const char *fallback, const char *suffix, bool create)
{
    const char *env = getenv(variable);
    const char *home = getenv("HOME");
    char *base = env && *env == '/' ? strdup(env) :
                 home && *home == '/' ? bp_join(home, fallback) : NULL;
    char *path = bp_join(base, suffix);
    free(base);
    if (create && path && !ensure_dirs(path)) { free(path); return NULL; }
    return path;
}

char *bp_cache_dir(bool create)
{
    return xdg("XDG_CACHE_HOME", ".cache", "burrow", create);
}

char *bp_data_dir(void)
{
    return xdg("XDG_DATA_HOME", ".local/share", "burrow", false);
}

FILE *bp_open_read(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    FILE *f = !fstat(fd, &st) && S_ISREG(st.st_mode) ? fdopen(fd, "rb") : NULL;
    if (!f) close(fd);
    return f;
}

static const char *publish(const char *path, bp_write_file write, void *context, bool replace)
{
    const char *slash = strrchr(path, '/');
    size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
    const char suffix[] = ".brw-stage-XXXXXX";
    char *stage = malloc(prefix + sizeof suffix);
    if (!stage) return "out-of-memory";
    memcpy(stage, path, prefix);
    memcpy(stage + prefix, suffix, sizeof suffix);
    const char *error = "stage-create";
    int fd = mkstemp(stage);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w+b");
        if (f) {
            error = write(f, context);
            if (fclose(f) && !error) error = "stage-close";
            if (!error && (replace ? rename(stage, path) : link(stage, path)))
                error = !replace && errno == EEXIST ? "source-exists" : "stage-publish";
        } else close(fd);
        if (error || !replace) unlink(stage);
    }
    free(stage);
    return error;
}

const char *bp_atomic_write(const char *path, bp_write_file write, void *context)
{
    return publish(path, write, context, true);
}

static const char *write_registration(FILE *file, void *context)
{
    return fputs(context, file) == EOF ? "source-write" : NULL;
}

int bp_source_add(const char *name, const char *input)
{
    char *catalog = realpath(input, NULL);
    const char *error = !bp_package_id(name) ? "invalid-source" :
                        !catalog ? "catalog-path" : bp_validate_catalog(name, catalog);
    char *root = NULL, *record = NULL;
    if (!error) {
        root = xdg("XDG_CONFIG_HOME", ".config", "burrow/sources", true);
        record = bp_join(root, name);
        if (record) {
            char *base = strrchr(record, '/') + 1;
            for (; *base; ++base) *base = (char)tolower((unsigned char)*base);
        }
        error = record ? publish(record, write_registration, catalog, false) : "source-directory";
    }
    free(record);
    free(root);
    free(catalog);
    if (error) fprintf(stderr, "brw: %s\n", error);
    else puts("added");
    return error ? 1 : 0;
}

const char *bp_catalogs(bp_catalog_visit visit, void *context)
{
    char *root = xdg("XDG_CONFIG_HOME", ".config", "burrow/sources", false);
    if (!root) return "source-directory";
    DIR *dir = opendir(root);
    if (!dir) {
        const char *error = errno == ENOENT ? NULL : "source-directory";
        free(root);
        return error;
    }
    const char *error = NULL;
    while (!error) {
        errno = 0;
        struct dirent *item = readdir(dir);
        if (!item) { if (errno) error = "source-directory"; break; }
        if (!bp_package_id(item->d_name)) continue;
        char *record = bp_join(root, item->d_name);
        FILE *f = record ? bp_open_read(record) : NULL;
        free(record);
        if (!f) { error = "source-record"; break; }
        char catalog[8192];
        size_t n = fread(catalog, 1, sizeof catalog, f);
        bool ok = n && n < sizeof catalog && !ferror(f) && catalog[0] == '/' &&
                  !memchr(catalog, 0, n);
        fclose(f);
        if (!ok) { error = "source-record"; break; }
        catalog[n] = 0;
        error = visit(item->d_name, catalog, context);
    }
    closedir(dir);
    free(root);
    return error;
}

const char *bp_fetch_artifact(const char *location, FILE *file)
{
    if (location[0] == '/') {
        FILE *input = bp_open_read(location);
        if (!input) return "download-open";
        unsigned char bytes[16384];
        size_t n;
        const char *error = NULL;
        while ((n = fread(bytes, 1, sizeof bytes, input)) != 0) {
            if (fwrite(bytes, 1, n, file) != n) { error = "download-stage"; break; }
        }
        if (ferror(input)) error = "download-read";
        fclose(input);
        return error;
    }
    char *const args[] = {"curl", "--disable", "--fail", "--silent", "--show-error", "--globoff",
                          "--proto", "=http,https", "--max-time", "300", "--", (char *)location, NULL};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) return "download-launch";
    int rc = posix_spawn_file_actions_adddup2(&actions, fileno(file), STDOUT_FILENO);
    pid_t pid;
    if (!rc) rc = posix_spawnp(&pid, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) return "download-launch";
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return "download-wait";
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? NULL : "download-failed";
}
