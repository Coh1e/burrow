#define _XOPEN_SOURCE 700
#include "blueprint_lock.h"
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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

/* Package and source identities are ASCII and case-insensitive. Use one
   spelling on disk even when catalog versions spell the same name differently. */
char *bp_package_child(const char *parent, const char *name)
{
    char *path = bp_join(parent, name);
    if (path) {
        char *leaf = strrchr(path, '/') + 1;
        for (; *leaf; ++leaf) *leaf = (char)tolower((unsigned char)*leaf);
    }
    return path;
}

/* Catalog entry names and staged archive paths have already been validated.
   The private staging tree contains only the directories and files made here. */
static int create_member(const char *stage, const char *name, bool executable)
{
    char *path = bp_join(stage, name);
    if (!path) return -1;
    char *leaf = strrchr(path, '/');
    *leaf = 0;
    bool ok = ensure_dirs(path);
    *leaf = '/';
    int fd = ok ? open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    free(path);
    if (fd >= 0 && fchmod(fd, executable ? 0700 : 0600)) {
        close(fd);
        return -1;
    }
    return fd;
}

static const char *unpack_tool(FILE *payload, const char *stage, const char *entry_name)
{
    struct archive *reader = archive_read_new();
    if (!reader) return "out-of-memory";
    const char *error = "archive-open";
    if (archive_read_support_format_tar(reader) == ARCHIVE_OK &&
        archive_read_support_format_zip(reader) == ARCHIVE_OK &&
        archive_read_support_filter_gzip(reader) == ARCHIVE_OK &&
        archive_read_open_FILE(reader, payload) == ARCHIVE_OK) {
        bool found = false;
        struct archive_entry *item;
        int status;
        error = NULL;
        while ((status = archive_read_next_header(reader, &item)) == ARCHIVE_OK) {
            const char *name = archive_entry_pathname(item);
            bool directory = archive_entry_filetype(item) == AE_IFDIR;
            if (!name || archive_entry_symlink(item) || archive_entry_hardlink(item) ||
                (!directory && archive_entry_filetype(item) != AE_IFREG)) {
                error = "archive-path";
                break;
            }
            while (!strncmp(name, "./", 2)) name += 2;
            if (directory && (!*name || !strcmp(name, "."))) continue;
            size_t length = strlen(name);
            if (directory && length && name[length - 1] == '/') --length;
            char relative[260];
            if (!length || length >= sizeof relative) { error = "archive-path"; break; }
            memcpy(relative, name, length);
            relative[length] = 0;
            if (!bp_safe_path(relative)) { error = "archive-path"; break; }
            if (directory) {
                char *path = bp_join(stage, relative);
                if (!path || !ensure_dirs(path)) error = "archive-output";
                free(path);
            } else {
                bool selected = !strcmp(relative, entry_name);
                int fd = create_member(stage, relative, selected || (archive_entry_perm(item) & 0111));
                if (fd < 0) error = "archive-output";
                else {
                    if (archive_read_data_into_fd(reader, fd) != ARCHIVE_OK) error = "archive-data";
                    if (close(fd) && !error) error = "archive-output";
                }
                if (selected) found = true;
            }
            if (error) break;
        }
        if (!error && status != ARCHIVE_EOF) error = "archive-data";
        if (!error && !found) error = "entry-missing";
    }
    if (archive_read_free(reader) != ARCHIVE_OK && !error) error = "archive-data";
    return error;
}

static const char *stage_tool(const struct bp_package *p, const char *cache, const char *stage)
{
    FILE *input = bp_open_read(cache);
    if (!input) return "cache-read";
    const char *error = NULL;
    if (!bp_matches_digest(input, p->digest) || fseek(input, 0, SEEK_SET)) error = "digest-mismatch";
    else if (!strcmp(p->kind, "archive")) error = unpack_tool(input, stage, p->entry);
    else {
        int fd = create_member(stage, p->entry, true);
        FILE *out = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!out) {
            if (fd >= 0) close(fd);
            error = "stage-create";
        } else {
            unsigned char bytes[16384];
            size_t n;
            while ((n = fread(bytes, 1, sizeof bytes, input)) != 0)
                if (fwrite(bytes, 1, n, out) != n) { error = "stage-write"; break; }
            if (ferror(input)) error = "cache-read";
            if (fclose(out) && !error) error = "stage-close";
        }
    }
    fclose(input);
    return error;
}

static bool same_file(const char *a, const char *b)
{
    FILE *one = bp_open_read(a), *two = bp_open_read(b);
    bool same = one && two;
    while (same) {
        unsigned char x[8192], y[8192];
        size_t n = fread(x, 1, sizeof x, one);
        same = fread(y, 1, sizeof y, two) == n && !memcmp(x, y, n) &&
               !ferror(one) && !ferror(two);
        if (n < sizeof x) break;
    }
    if (one) fclose(one);
    if (two) fclose(two);
    return same;
}

/* Existing versions can be used only when their entire tree matches the
   locked payload, including executable permissions and additional siblings. */
static bool same_tool_tree(const char *a, const char *b)
{
    struct stat left, right;
    if (lstat(a, &left) || lstat(b, &right)) return false;
    if (S_ISREG(left.st_mode)) {
        return S_ISREG(right.st_mode) && !(right.st_mode & (S_ISUID | S_ISGID)) &&
               ((left.st_mode ^ right.st_mode) & S_IXUSR) == 0 &&
               left.st_size == right.st_size && same_file(a, b);
    }
    if (!S_ISDIR(left.st_mode) || !S_ISDIR(right.st_mode)) return false;
    DIR *staged = opendir(a), *installed = opendir(b);
    bool same = staged && installed;
    size_t expected = 0, actual = 0;
    struct dirent *item;
    while (same) {
        errno = 0;
        item = readdir(staged);
        if (!item) { same = !errno; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char *x = bp_join(a, item->d_name), *y = bp_join(b, item->d_name);
        same = x && y && same_tool_tree(x, y);
        free(x);
        free(y);
        ++expected;
    }
    if (same) {
        errno = 0;
        while ((item = readdir(installed)) != NULL)
            if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..")) ++actual;
        same = !errno && expected == actual;
    }
    if (staged) closedir(staged);
    if (installed) closedir(installed);
    return same;
}

static int remove_staged(const char *path, const struct stat *st, int type, struct FTW *info)
{
    (void)st;
    (void)type;
    (void)info;
    return remove(path);
}

static bool package_directory(const char *path)
{
    struct stat st;
    if (!path || (mkdir(path, 0700) && errno != EEXIST)) return false;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static char *package_base(const struct bp_package *p, bool create)
{
    char *data = bp_data_dir(), *packages = bp_join(data, "packages");
    char *base = bp_package_child(packages, p->name);
    if (create && base && (!ensure_dirs(data) || !package_directory(packages) ||
                           !package_directory(base))) {
        free(base);
        base = NULL;
    }
    free(packages);
    free(data);
    return base;
}

const char *bp_materialize_tool(const struct bp_package *p, const char *cache, char **program)
{
    *program = NULL;
    char *base = package_base(p, true);
    char *version = bp_join(base, p->version), *entry = bp_join(version, p->entry);
    char *stage = bp_join(base, ".brw-tool-XXXXXX");
    const char *error = "data-directory";
    if (entry && stage) {
        error = "stage-create";
        if (mkdtemp(stage)) {
            error = stage_tool(p, cache, stage);
            /* A complete version contains its entry, so two simultaneous
               publishers cannot replace each other's nonempty directory. */
            if (!error) {
                struct stat st;
                if (!lstat(version, &st)) {
                    if (!same_tool_tree(stage, version)) error = "version-conflict";
                } else if (errno != ENOENT) error = "version-inspect";
                else if (rename(stage, version)) {
                    if (!same_tool_tree(stage, version)) error = "stage-publish";
                } else stage[0] = 0;
            }
            if (*stage && nftw(stage, remove_staged, 16, FTW_DEPTH | FTW_PHYS) && !error)
                error = "cleanup-stage";
        }
    }
    if (!error) *program = entry;
    else free(entry);
    free(stage);
    free(version);
    free(base);
    return error;
}

const char bp_home_key[] = "HOME";

bool bp_env_key_equal(const char *name, const char *key)
{
    return !strcmp(name, key);
}

static char *bin_directory(const char *bin, const char *home)
{
    if (!bin) bin = getenv("BRW_BIN_DIR");
    if (bin && *bin == '/') return strdup(bin);
    if (!home) home = getenv(bp_home_key);
    return home && *home == '/' ? bp_join(home, ".local/bin") : NULL;
}

char *bp_default_entry(const struct bp_package *p, const char *bin_override, const char *home_override)
{
    char *bin = bin_directory(bin_override, home_override);
    char *entry = bp_package_child(bin, p->name);
    free(bin);
    return entry;
}

/* An existing entry belongs to this package only if it links into one of
   this package's version directories. A user's unrelated entry is retained. */
static bool package_link(char *target, const char *base)
{
    size_t n = strlen(base);
    if (strncmp(target, base, n) || target[n] != '/') return false;
    char *version = target + n + 1, *entry = strchr(version, '/');
    if (!entry) return false;
    *entry++ = 0;
    if (!bp_package_id(version)) return false;
    for (;;) {
        char *next = strchr(entry, '/');
        if (next) *next++ = 0;
        if (!bp_package_id(entry)) return false;
        if (!next) return true;
        entry = next;
    }
}

enum link_state { LINK_ABSENT, LINK_OTHER_VERSION, LINK_SELECTED };

static const char *inspect_link(const char *entry, const char *base, const char *program,
                                enum link_state *state)
{
    *state = LINK_ABSENT;
    /* No valid installed link exceeds the package prefix, version, and entry limits. */
    size_t cap = strlen(base) + 1 + 128 + 1 + 260 + 1;
    char *target = malloc(cap);
    if (!target) return "out-of-memory";
    ssize_t n = readlink(entry, target, cap);
    const char *error = "entry-conflict";
    if (n < 0 && errno == ENOENT) error = NULL;
    else if (n >= 0 && (size_t)n < cap) {
        target[n] = 0;
        if (!strcmp(target, program)) {
            *state = LINK_SELECTED;
            error = NULL;
        } else if (package_link(target, base)) {
            *state = LINK_OTHER_VERSION;
            error = NULL;
        }
    }
    free(target);
    return error;
}

const char *bp_default_matches(const struct bp_package *p, bool *matches)
{
    char *entry = bp_default_entry(p, NULL, NULL), *program = bp_version_entry(p), *base = package_base(p, false);
    enum link_state state = LINK_ABSENT;
    const char *error = entry && program && base ? inspect_link(entry, base, program, &state)
                                                 : "entry-directory";
    struct stat info;
    *matches = !error && state == LINK_SELECTED && !lstat(program, &info) &&
               S_ISREG(info.st_mode) && access(program, X_OK) == 0;
    free(base);
    free(program);
    free(entry);
    return error;
}

static const char *switch_link(const char *bin, const char *entry, const char *base,
                               const char *program, bool *changed)
{
    enum link_state state;
    const char *error = inspect_link(entry, base, program, &state);
    if (error || state == LINK_SELECTED) return error;
    if (state == LINK_ABSENT) {
        if (symlink(program, entry)) return "entry-publish";
        *changed = true;
        return NULL;
    }
    char *stage = bp_join(bin, ".brw-entry-XXXXXX");
    error = "stage-create";
    if (stage && mkdtemp(stage)) {
        char *link = bp_join(stage, "next");
        if (link && !symlink(program, link)) {
            error = rename(link, entry) ? "entry-publish" : NULL;
            if (error) unlink(link);
            else *changed = true;
        }
        free(link);
        if (rmdir(stage) && !error) error = "cleanup-stage";
    }
    free(stage);
    return error;
}

const char *bp_set_default(const struct bp_package *p, const char *cache, bool *changed)
{
    *changed = false;
    char *program = NULL;
    const char *error = bp_materialize_tool(p, cache, &program);
    if (error) return error;
    char *base = package_base(p, false);
    char *bin = bin_directory(NULL, NULL), *entry = bp_package_child(bin, p->name);
    int dir = base && entry && ensure_dirs(bin) ? open(bin, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    error = "bin-directory";
    if (dir >= 0) {
        int rc;
        do { rc = flock(dir, LOCK_EX); } while (rc && errno == EINTR);
        error = rc ? "entry-lock" : switch_link(bin, entry, base, program, changed);
        close(dir);
    }
    free(entry);
    free(bin);
    free(base);
    free(program);
    return error;
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
        record = bp_package_child(root, name);
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
