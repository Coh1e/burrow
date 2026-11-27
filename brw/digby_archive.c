#include "digby_archive.h"
#include "digby_archive_io.h"
#include "verified_file.h"
#include <archive.h>
#include <archive_entry.h>
#include <stdlib.h>
#include <string.h>

enum { MEMBER_LIMIT = 4096, EXPANDED_LIMIT = 256 * 1024 * 1024 };

struct member {
    char *path;
    bool directory, explicit_entry;
};

struct extraction {
    struct member *members; /* Sorted by case-insensitive portable path. */
    size_t count, capacity, expanded;
    struct digby_archive_stage *stage; /* NULL in preview. */
};

static unsigned fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned)c + ('a' - 'A') : c;
}

static int compare_names(const char *a, const char *b)
{
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { ++a; ++b; }
    return (int)fold((unsigned char)*a) - (int)fold((unsigned char)*b);
}

static const char *record_member(struct extraction *tree, const char *path,
                                 bool directory, bool explicit_entry)
{
    size_t lo = 0, hi = tree->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        struct member *entry = &tree->members[middle];
        int cmp = compare_names(path, entry->path);
        if (!cmp) {
            if (strcmp(path, entry->path) || directory != entry->directory ||
                (explicit_entry && entry->explicit_entry)) return "archive-path-conflict";
            if (explicit_entry) entry->explicit_entry = true;
            return NULL;
        }
        if (cmp < 0) hi = middle;
        else lo = middle + 1;
    }
    if (tree->count == MEMBER_LIMIT) return "archive-limit";
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity ? tree->capacity * 2 : 16;
        struct member *members = realloc(tree->members, capacity * sizeof *members);
        if (!members) return "out-of-memory";
        tree->capacity = capacity;
        tree->members = members;
    }
    char *copy = malloc(strlen(path) + 1);
    if (!copy) return "out-of-memory";
    strcpy(copy, path);
    memmove(&tree->members[lo + 1], &tree->members[lo], (tree->count - lo) * sizeof *tree->members);
    tree->members[lo] = (struct member){copy, directory, explicit_entry};
    ++tree->count;
    return NULL;
}

/* An entry can create implicit directories. Record them too, so a later
   file or different spelling cannot claim one, even during preview. */
static const char *record_path(struct extraction *tree, char *path, bool directory)
{
    for (char *p = path; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        const char *error = record_member(tree, path, true, false);
        *p = '/';
        if (error) return error;
    }
    return record_member(tree, path, directory, true);
}

static const char *read_body(struct archive *reader, FILE *out, size_t expected)
{
    unsigned char block[16384];
    la_ssize_t n;
    size_t remaining = expected;
    while ((n = archive_read_data(reader, block, sizeof block)) > 0) {
        if ((size_t)n > remaining) return "archive-data";
        if (out && fwrite(block, 1, (size_t)n, out) != (size_t)n) return "archive-output";
        remaining -= (size_t)n;
    }
    return n == 0 && !remaining ? NULL : "archive-data";
}

static const char *read_member(struct archive *reader, struct archive_entry *item, struct extraction *tree)
{
    bool directory = archive_entry_filetype(item) == AE_IFDIR;
    const char *name = archive_entry_pathname(item);
    if ((!directory && archive_entry_filetype(item) != AE_IFREG) || !name || !*name ||
        archive_entry_symlink(item) || archive_entry_hardlink(item)) return "archive-path";
    if (archive_entry_is_encrypted(item) || !archive_entry_size_is_set(item)) return "archive-data";
    la_int64_t size = archive_entry_size(item);
    if (size < 0 || (directory && size)) return "archive-data";
    if (size > (la_int64_t)(EXPANDED_LIMIT - tree->expanded)) return "archive-limit";

    while (!strncmp(name, "./", 2)) name += 2;
    if (directory && (!*name || !strcmp(name, "."))) return NULL;
    size_t length = strlen(name);
    if (directory && length && name[length - 1] == '/') --length;
    char path[BP_PACKAGE_PATH_CAP];
    if (!length || length >= sizeof path) return "archive-path";
    memcpy(path, name, length);
    path[length] = 0;
    if (!bp_safe_package_path(path)) return "archive-path";
    const char *error = record_path(tree, path, directory);
    FILE *out = NULL;
    if (!error && tree->stage)
        error = digby_archive_entry(tree->stage, path, directory, (archive_entry_perm(item) & 0111) != 0, &out);
    if (!error && !directory) error = read_body(reader, out, (size_t)size);
    if (out && fclose(out) && !error) error = "archive-output";
    tree->expanded += (size_t)size;
    return error;
}

static int enable_zip(struct archive *reader, const struct digby_archive_plan *plan)
{
    /* libarchive's seekable reader requires a central directory for ZIP and
       verifies its entries. An empty ZIP consists only of an end record and
       optional comment, with the disk, entry, size and offset fields zero. */
    static const unsigned char empty_end[20] = {'P', 'K', 5, 6};
    if (plan->size >= 22 && !memcmp(plan->data, empty_end, sizeof empty_end) &&
        plan->size - 22 == (size_t)plan->data[20] + 256u * plan->data[21])
        return archive_read_support_format_zip_streamable(reader);
    return archive_read_support_format_zip_seekable(reader);
}

static const char *check_operands(const struct digby_archive_plan *plan)
{
    if (!plan || !plan->data || !plan->workspace || !*plan->workspace) return "invalid-archive-plan";
    return bp_safe_path(plan->destination) ? NULL : "unsafe-destination";
}

static const char *scan(const struct digby_archive_plan *plan, struct digby_archive_stage *stage)
{
    struct archive *reader = archive_read_new();
    if (!reader) return "out-of-memory";
    const char *error = NULL;
    struct extraction tree = {.stage = stage};
    if (archive_read_support_format_tar(reader) != ARCHIVE_OK ||
        enable_zip(reader, plan) != ARCHIVE_OK ||
        archive_read_support_filter_gzip(reader) != ARCHIVE_OK ||
        archive_read_open_memory(reader, plan->data, plan->size) != ARCHIVE_OK) error = "archive-data";

    size_t count = 0;
    while (!error) {
        struct archive_entry *item = NULL;
        int status = archive_read_next_header(reader, &item);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK) error = "archive-data";
        else if (++count > MEMBER_LIMIT) error = "archive-limit";
        else error = read_member(reader, item, &tree);
    }
    if (archive_read_free(reader) != ARCHIVE_OK && !error) error = "archive-data";
    for (size_t i = 0; i < tree.count; ++i) free(tree.members[i].path);
    free(tree.members);
    return error;
}

const char *digby_archive_validate(const struct digby_archive_plan *plan)
{
    const char *error = check_operands(plan);
    return error ? error : scan(plan, NULL);
}

const char *digby_archive_extract(const struct digby_archive_plan *plan, bool preview)
{
    const char *error = check_operands(plan);
    if (error) return error;
    struct digby_archive_stage *stage = NULL;
    error = preview ? digby_archive_destination(plan->workspace, plan->destination) :
                      digby_archive_begin(plan->workspace, plan->destination, &stage);
    if (error) return error;
    error = scan(plan, stage);
    if (stage) {
        const char *finish = digby_archive_finish(stage, !error);
        if (finish) return finish;
    }
    return error;
}
