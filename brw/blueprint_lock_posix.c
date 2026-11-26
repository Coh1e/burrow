#define _XOPEN_SOURCE 700
#include "blueprint_lock.h"
#include "sha.h"
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* A read-only inventory lets preview compare the whole installed tree with
   the archive, including additional files and implicit parent directories. */
struct member {
    char *name;
    mode_t mode;
    dev_t device;
    ino_t inode;
    bool seen;
};

struct inventory {
    const char *root;
    struct member *members;
    size_t count, capacity;
};

static bool inventory_directory(struct inventory *tree, const char *dir, const char *prefix)
{
    struct stat st;
    DIR *stream = !lstat(dir, &st) && S_ISDIR(st.st_mode) ? opendir(dir) : NULL;
    if (!stream) return false;
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *ent = readdir(stream);
        if (!ent) { ok = !errno; break; }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *relative = prefix ? bp_join(prefix, ent->d_name) : strdup(ent->d_name);
        char *path = bp_join(dir, ent->d_name);
        ok = relative && path && bp_safe_package_path(relative) && !lstat(path, &st) &&
             (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && !(st.st_mode & (S_ISUID | S_ISGID))));
        if (ok && tree->count == tree->capacity) {
            size_t n = tree->capacity ? tree->capacity * 2 : 16;
            struct member *more = realloc(tree->members, n * sizeof *more);
            if (!more) ok = false;
            else { tree->members = more; tree->capacity = n; }
        }
        if (ok) {
            tree->members[tree->count++] = (struct member){
                .name = relative, .mode = st.st_mode, .device = st.st_dev, .inode = st.st_ino};
            if (S_ISDIR(st.st_mode)) ok = inventory_directory(tree, path, relative);
        } else free(relative);
        free(path);
    }
    closedir(stream);
    return ok;
}

static int compare_members(const void *a, const void *b)
{
    const struct member *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

/* An archive may spell a shared parent differently in two headers. Let the
   mounted filesystem decide which spellings refer to the same installed file. */
static struct member *find_member(struct inventory *tree, char *name)
{
    struct member key = {.name = name};
    struct member *m = tree->count ? bsearch(&key, tree->members, tree->count,
                                              sizeof key, compare_members) : NULL;
    if (!m || !strcmp(m->name, name)) return m;
    char *path = bp_join(tree->root, name);
    struct stat st;
    bool found = path && !lstat(path, &st);
    free(path);
    if (!found) return NULL;
    while (m > tree->members && !strcasecmp(m[-1].name, name)) --m;
    struct member *alias = NULL;
    for (; m < tree->members + tree->count && !strcasecmp(m->name, name); ++m) {
        if (!strcmp(m->name, name)) return m;
        if (m->device == st.st_dev && m->inode == st.st_ino) alias = m;
    }
    return alias;
}

static bool mark_member(struct inventory *tree, char *name, bool directory, bool executable)
{
    /* An archive can omit explicit headers for the parents of a file. */
    char *end = name;
    do {
        end = strchr(end, '/');
        if (end) *end = 0;
        struct member *m = find_member(tree, name);
        bool ok = m && (end || directory ? S_ISDIR(m->mode) :
                        S_ISREG(m->mode) && !m->seen && !!(m->mode & S_IXUSR) == executable);
        if (end) *end++ = '/';
        if (!ok) return false;
        m->seen = true;
    } while (end);
    return true;
}

static bool archive_file_matches(struct archive *reader, const char *directory, const char *member)
{
    char *path = bp_join(directory, member);
    FILE *file = path ? bp_open_read(path) : NULL;
    free(path);
    if (!file) return false;
    unsigned char original[16384], installed[sizeof original];
    la_ssize_t n;
    bool same = true;
    while ((n = archive_read_data(reader, original, sizeof original)) > 0) {
        if (fread(installed, 1, (size_t)n, file) != (size_t)n ||
            memcmp(original, installed, (size_t)n)) { same = false; break; }
    }
    same = same && !n && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return same;
}

static const char *read_tool_archive(FILE *payload, const char *stage, const char *entry_name,
                                     struct inventory *verify)
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
            char relative[BP_PACKAGE_PATH_CAP];
            if (!length || length >= sizeof relative) { error = "archive-path"; break; }
            memcpy(relative, name, length);
            relative[length] = 0;
            if (!bp_safe_package_path(relative)) { error = "archive-path"; break; }
            bool selected = !directory && !strcmp(relative, entry_name);
            bool executable = selected || (archive_entry_perm(item) & 0111);
            if (verify) {
                if (!mark_member(verify, relative, directory, executable) ||
                    (!directory && !archive_file_matches(reader, stage, relative))) error = "version-conflict";
            } else if (directory) {
                char *path = bp_join(stage, relative);
                if (!path || !ensure_dirs(path)) error = "archive-output";
                free(path);
            } else {
                int fd = create_member(stage, relative, executable);
                if (fd < 0) error = "archive-output";
                else {
                    if (archive_read_data_into_fd(reader, fd) != ARCHIVE_OK) error = "archive-data";
                    if (close(fd) && !error) error = "archive-output";
                }
            }
            if (selected) found = true;
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
    else if (!strcmp(p->kind, "archive")) error = read_tool_archive(input, stage, p->entry, NULL);
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

/* Compare the installed files to the locked artifact without creating a staging
   directory. Apply and preview share exactly the same conflict checks. */
static const char *verify_version(const struct bp_package *p, const char *cache, const char *dir)
{
    struct inventory tree = {.root = dir};
    const char *error = inventory_directory(&tree, dir, NULL) ? NULL : "version-conflict";
    if (!error && tree.count) qsort(tree.members, tree.count, sizeof *tree.members, compare_members);
    if (!error && !strcmp(p->kind, "exe")) {
        char *path = bp_join(dir, p->entry);
        FILE *file = path ? bp_open_read(path) : NULL;
        char entry[BP_PACKAGE_PATH_CAP];
        strcpy(entry, p->entry); /* Validated by the catalog/lock reader. */
        if (!file || !mark_member(&tree, entry, false, true) || !bp_matches_digest(file, p->digest))
            error = "version-conflict";
        if (file) fclose(file);
        free(path);
    } else if (!error) {
        FILE *file = cache ? bp_open_read(cache) : NULL;
        if (!file) error = "cache-read";
        else {
            error = bp_matches_digest(file, p->digest) && !fseek(file, 0, SEEK_SET) ?
                    read_tool_archive(file, dir, p->entry, &tree) : "digest-mismatch";
            fclose(file);
        }
    }
    for (size_t i = 0; i < tree.count; ++i) {
        if (!tree.members[i].seen && !error) error = "version-conflict";
        free(tree.members[i].name);
    }
    free(tree.members);
    return error;
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

static const char *inspect_version(const struct bp_package *p, const char *cache, const char *version,
                                   bool *present)
{
    *present = false;
    struct stat st;
    if (!version) return "data-directory";
    if (lstat(version, &st)) return errno == ENOENT ? NULL : "version-inspect";
    if (!S_ISDIR(st.st_mode)) return "version-conflict";
    /* Apply may fetch missing archive bytes; preview cannot confirm a match yet. */
    if (!cache && strcmp(p->kind, "exe")) return NULL;
    const char *error = verify_version(p, cache, version);
    *present = !error;
    return error;
}

const char *bp_check_tool(const struct bp_package *p, const char *cache, bool *present)
{
    char *base = package_base(p, false), *version = bp_join(base, p->version);
    const char *error = inspect_version(p, cache, version, present);
    free(version);
    free(base);
    return error;
}

const char *bp_materialize_tool(const struct bp_package *p, const char *cache, char **program)
{
    *program = NULL;
    char *base = package_base(p, true);
    char *version = bp_join(base, p->version), *entry = bp_join(version, p->entry);
    char *stage = bp_join(base, ".brw-tool-XXXXXX");
    const char *error = "data-directory";
    if (entry && stage) {
        struct stat st;
        if (!lstat(version, &st)) error = verify_version(p, cache, version);
        else if (errno != ENOENT) error = "version-inspect";
        else if (!mkdtemp(stage)) error = "stage-create";
        else {
            error = stage_tool(p, cache, stage);
            /* A competing successful publication leaves a nonempty version.
               Verify its contents against our lock before using it. */
            if (!error && !rename(stage, version)) stage[0] = 0;
            else if (!error) error = verify_version(p, cache, version);
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

/* Launch through the actual package path so scripts can find their siblings.
   Single quotes protect workspace paths, including embedded quotes. */
static void shell_literal(FILE *f, const char *value)
{
    fputc('\'', f);
    for (; *value; ++value) {
        if (*value == '\'') fputs("'\\''", f);
        else fputc(*value, f);
    }
    fputc('\'', f);
}

struct launcher {
    char *script;
    size_t size;
    char digest[65];
};

static bool make_launcher(const char *program, const char *child_path, struct launcher *launcher)
{
    FILE *out = open_memstream(&launcher->script, &launcher->size);
    if (!out) return false;
    fputs("#!/bin/sh\n", out);
    if (child_path) {
        fputs("export PATH=", out);
        shell_literal(out, child_path);
        fputs("\"${PATH+:$PATH}\"\n", out);
    }
    fputs("exec ", out);
    shell_literal(out, program);
    fputs(" \"$@\"\n", out);
    bool ok = !ferror(out);
    if (fclose(out) || !ok) return false;
    SHA256Context ctx;
    unsigned char digest[SHA256HashSize];
    if (launcher->size > UINT_MAX || SHA256Reset(&ctx) ||
        SHA256Input(&ctx, (const unsigned char *)launcher->script, (unsigned)launcher->size) ||
        SHA256Result(&ctx, digest)) return false;
    for (size_t i = 0; i < sizeof digest; ++i)
        snprintf(launcher->digest + 2*i, 3, "%02x", digest[i]);
    return true;
}

static bool matching_launcher(const char *target, const char *directory)
{
    size_t n = strlen(directory);
    if (strncmp(target, directory, n) || target[n] != '/') return false;
    const char *digest = target + n + 1;
    struct stat st;
    if (strlen(digest) != 64 || strspn(digest, "0123456789abcdef") != 64 ||
        lstat(directory, &st) || !S_ISDIR(st.st_mode)) return false;
    FILE *file = bp_open_read(target);
    bool matches = file && !fstat(fileno(file), &st) && (st.st_mode & S_IXUSR) &&
                   !(st.st_mode & (S_ISUID | S_ISGID)) && bp_matches_digest(file, digest);
    if (file) fclose(file);
    return matches;
}

static const char *write_launcher(FILE *file, void *context)
{
    const struct launcher *launcher = context;
    return fwrite(launcher->script, 1, launcher->size, file) == launcher->size &&
           !fchmod(fileno(file), 0700) ? NULL : "entry-publish";
}

static const char *publish(const char *path, bp_write_file write, void *context, bool replace);

static const char *prepare_launcher(const char *directory, const char *target, struct launcher *launcher)
{
    if (!package_directory(directory)) return "entry-directory";
    if (matching_launcher(target, directory)) return NULL;
    const char *error = publish(target, write_launcher, launcher, false);
    if (error && !strcmp(error, "source-exists"))
        return matching_launcher(target, directory) ? NULL : "entry-conflict";
    return error;
}

static bool legacy_entry(const struct bp_package *p, const char *base, char *target)
{
    size_t n = strlen(base);
    if (strncmp(target, base, n) || target[n] != '/') return false;
    char *version = target + n + 1, *entry = strchr(version, '/');
    if (!entry) return false;
    *entry++ = 0;
    return bp_package_id(version) && bp_registered_entry(p, version, entry);
}

enum link_state { LINK_ABSENT, LINK_OTHER_VERSION, LINK_SELECTED };

static const char *inspect_link(const struct bp_package *p, const char *entry, const char *base,
                                const char *program, const char *launchers, const char *expected,
                                enum link_state *state)
{
    *state = LINK_ABSENT;
    size_t cap = strlen(base) + 1 + 128 + 1 + BP_PACKAGE_PATH_CAP;
    char *target = malloc(cap);
    if (!target) return "out-of-memory";
    ssize_t n = readlink(entry, target, cap);
    const char *error = "entry-conflict";
    if (n < 0 && errno == ENOENT) error = NULL;
    else if (n >= 0 && (size_t)n < cap) {
        target[n] = 0;
        if (matching_launcher(target, launchers)) {
            *state = !strcmp(target, expected) ? LINK_SELECTED : LINK_OTHER_VERSION;
            error = NULL;
        } else if (!strcmp(target, program) || legacy_entry(p, base, target)) {
            /* Upgrade direct entry links after confirming the declared target. */
            *state = LINK_OTHER_VERSION;
            error = NULL;
        }
    }
    free(target);
    return error;
}

static const char *switch_link(const char *bin, const char *entry, const char *target,
                               enum link_state state)
{
    if (state == LINK_SELECTED) return NULL;
    if (state == LINK_ABSENT) return symlink(target, entry) ? "entry-publish" : NULL;
    char *stage = bp_join(bin, ".brw-entry-XXXXXX");
    const char *error = "stage-create";
    if (stage && mkdtemp(stage)) {
        char *link = bp_join(stage, "next");
        if (link && !symlink(target, link)) {
            error = rename(link, entry) ? "entry-publish" : NULL;
            if (error) unlink(link);
        }
        free(link);
        if (rmdir(stage) && !error) error = "cleanup-stage";
    }
    free(stage);
    return error;
}

static const char *default_entry_state(const struct bp_package *p, const char *cache,
                                       const char *child_path, bool apply, bool *result)
{
    *result = false;
    char *base = package_base(p, false), *version = bp_join(base, p->version);
    char *program = bp_join(version, p->entry), *launchers = bp_join(base, ".brw-launchers");
    char *bin = bin_directory(NULL, NULL), *entry = bp_package_child(bin, p->name);
    struct launcher launcher = {0};
    char *target = NULL;
    const char *error = "entry-directory";
    int dir = -1;
    if (!entry || !program || !launchers || !make_launcher(program, child_path, &launcher)) goto done;
    target = bp_join(launchers, launcher.digest);
    if (!target) { error = "out-of-memory"; goto done; }
    if (apply) {
        if (!ensure_dirs(bin) || (dir = open(bin, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) goto done;
        int rc;
        do { rc = flock(dir, LOCK_EX); } while (rc && errno == EINTR);
        if (rc) { error = "entry-lock"; goto done; }
    }
    enum link_state state;
    error = inspect_link(p, entry, base, program, launchers, target, &state);
    if (error) goto done;
    if (apply) {
        struct stat info;
        bool exists = !lstat(version, &info);
        char *installed = NULL;
        error = bp_materialize_tool(p, cache, &installed);
        free(installed);
        if (!error) error = prepare_launcher(launchers, target, &launcher);
        if (!error) error = switch_link(bin, entry, target, state);
        *result = !error && (state != LINK_SELECTED || !exists);
    } else {
        bool present;
        error = inspect_version(p, cache, version, &present);
        *result = present && state == LINK_SELECTED;
    }
done:
    if (dir >= 0) close(dir);
    free(launcher.script);
    free(target);
    free(entry);
    free(bin);
    free(launchers);
    free(program);
    free(version);
    free(base);
    return error;
}

const char *bp_default_matches(const struct bp_package *p, const char *cache, const char *child_path,
                               bool *matches)
{
    return default_entry_state(p, cache, child_path, false, matches);
}

const char *bp_set_default(const struct bp_package *p, const char *cache, const char *child_path,
                           bool *changed)
{
    return default_entry_state(p, cache, child_path, true, changed);
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
