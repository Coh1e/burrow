#include "blueprint_lock.h"
#include "sha.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_CAP = 8192, MAX_CANDIDATES = 65536 };

static char *copy(const char *s)
{
    char *result = malloc(strlen(s) + 1);
    if (result) strcpy(result, s);
    return result;
}

char *bp_join(const char *dir, const char *name)
{
    if (!dir) return NULL;
    size_t size = strlen(dir) + strlen(name) + 2;
    char *result = malloc(size);
    if (result) snprintf(result, size, "%s/%s", dir, name);
    return result;
}

bool bp_package_id(const char *s)
{
    if (!s || !bp_safe_path(s) || *s == '.' || strlen(s) >= 64) return false;
    return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(s);
}

static bool equal(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}

static bool hex_digest(const char *s)
{
    return strlen(s) == 64 && strspn(s, "abcdefABCDEF0123456789") == 64;
}

/* Numeric runs compare by magnitude even when larger than a machine integer. */
static int version_order(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            size_t an = strspn(a, "0123456789"), bn = strspn(b, "0123456789");
            while (an > 1 && *a == '0') { ++a; --an; }
            while (bn > 1 && *b == '0') { ++b; --bn; }
            if (an != bn) return an > bn ? 1 : -1;
            int order = strncmp(a, b, an);
            if (order) return order;
            a += an;
            b += bn;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            ++a; ++b;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool compatible(const char *requirement, const char *version)
{
    size_t n = strlen(requirement);
    if (!strcmp(requirement, "*")) return true;
    if (n > 2 && !strcmp(requirement + n - 2, ".*"))
        return !strncmp(requirement, version, n - 1) && version[n - 1];
    return !strcmp(requirement, version);
}

static bool valid_requirement(const char *s)
{
    if (!strcmp(s, "*") || bp_package_id(s)) return true;
    size_t n = strlen(s);
    if (n < 3 || n >= 64 || strcmp(s + n - 2, ".*")) return false;
    char prefix[64];
    memcpy(prefix, s, n - 2);
    prefix[n - 2] = 0;
    return bp_package_id(prefix);
}

static char *take(char **rest, char delimiter)
{
    char *field = *rest;
    if (!field) return NULL;
    *rest = strchr(field, delimiter);
    if (*rest) *(*rest)++ = 0;
    return field;
}

static bool dependencies(const char *s)
{
    while (*s) {
        size_t n = strcspn(s, ",");
        char name[64];
        if (n >= sizeof name) return false;
        memcpy(name, s, n);
        name[n] = 0;
        if (!bp_package_id(name)) return false;
        s += n;
        if (*s && !*++s) return false;
    }
    return true;
}

struct catalog {
    struct bp_package *rows;
    size_t count, capacity;
};

static const char *add_row(struct catalog *db, const char *source, const char *line)
{
    if (!bp_package_id(source)) return "catalog-format";
    if (db->count == db->capacity) {
        if (db->capacity == MAX_CANDIDATES) return "catalog-limit";
        size_t n = db->capacity ? db->capacity * 2 : 16;
        struct bp_package *rows = realloc(db->rows, n * sizeof *rows);
        if (!rows) return "out-of-memory";
        db->rows = rows;
        db->capacity = n;
    }
    struct bp_package row = {0};
    row.storage = malloc(strlen(source) + strlen(line) + 2);
    if (!row.storage) return "out-of-memory";
    row.registry = strcpy(row.storage, source);
    char *fields = strcpy(row.storage + strlen(source) + 1, line);
    row.name = take(&fields, '\t');
    row.version = take(&fields, '\t');
    row.kind = take(&fields, '\t');
    row.location = take(&fields, '\t');
    row.digest = take(&fields, '\t');
    row.entry = take(&fields, '\t');
    row.depends = fields ? fields : "";
    if (!bp_package_id(row.name) || !bp_package_id(row.version) || !row.entry ||
        (strcmp(row.kind, "exe") && strcmp(row.kind, "archive")) || !hex_digest(row.digest) ||
        (!bp_package_id(row.location) && strncmp(row.location, "https://", 8) &&
         strncmp(row.location, "http://", 7)) || !bp_safe_path(row.entry) || !dependencies(row.depends)) {
        free(row.storage);
        return "catalog-format";
    }
    db->rows[db->count++] = row;
    return NULL;
}

static int read_line(FILE *file, char line[LINE_CAP])
{
    size_t n = 0;
    int c;
    while ((c = fgetc(file)) != EOF && c != '\n') {
        if (!c || n + 1 == LINE_CAP) return -1;
        line[n++] = (char)c;
    }
    if (ferror(file)) return -1;
    if (n && line[n - 1] == '\r') --n;
    line[n] = 0;
    return c == EOF && !n ? 0 : 1;
}

static const char *read_rows(struct catalog *db, FILE *file, const char *header, const char *source)
{
    char line[LINE_CAP];
    const char *format = source ? "catalog-format" : "lock-format";
    if (read_line(file, line) != 1 || strcmp(line, header)) return format;
    int rc;
    while ((rc = read_line(file, line)) == 1) {
        char *fields = line;
        const char *registry = source ? source : take(&fields, '\t');
        if (!fields) return format;
        const char *error = add_row(db, registry, fields);
        if (error) return !strcmp(error, "catalog-format") ? format : error;
    }
    return rc < 0 ? format : NULL;
}

static const char *load_catalog(const char *source, const char *path, void *context)
{
    FILE *f = bp_open_read(path);
    if (!f) return "catalog-open";
    const char *error = read_rows(context, f, "brw-catalog-v1", source);
    fclose(f);
    return error;
}

const char *bp_validate_catalog(const char *source, const char *path)
{
    struct catalog db = {0};
    const char *error = load_catalog(source, path, &db);
    for (size_t i = 0; i < db.count; ++i) free(db.rows[i].storage);
    free(db.rows);
    return error;
}

struct resolution {
    const struct bp_plan *plan;
    struct catalog candidates;
    struct bp_package *order[BP_MAX_STEPS];
    const char *chain[BP_MAX_STEPS];
    size_t count;
    const char *platform;
    char *detail;
    bool generate;
};

static bool allowed(const struct bp_plan *plan, const struct bp_package *row)
{
    for (size_t i = 0; i < plan->count; ++i) {
        const struct bp_step *s = &plan->steps[i];
        if (s->op == BP_TOOL && equal(s->name, row->name) &&
            ((s->registry && !equal(s->registry, row->registry)) ||
             !compatible(s->version, row->version))) return false;
    }
    return true;
}

static const char *resolve(struct resolution *r, const char *name, size_t step, size_t depth)
{
    for (size_t i = 0; i < r->count; ++i)
        if (equal(r->order[i]->name, name)) return NULL;
    snprintf(r->detail, BP_DETAIL_CAP, "%s (%s)", name, r->platform);
    if (depth == BP_MAX_STEPS) return "dependency-depth";
    for (size_t i = 0; i < depth; ++i)
        if (equal(r->chain[i], name)) return "dependency-cycle";
    struct bp_package *best = NULL;
    bool found = false, tie = false;
    for (size_t i = 0; i < r->candidates.count; ++i) {
        struct bp_package *row = &r->candidates.rows[i];
        if (!equal(row->name, name)) continue;
        if (found && !r->generate) return "lock-duplicate";
        found = true;
        if (!allowed(r->plan, row)) continue;
        if (best && !equal(best->registry, row->registry)) return "ambiguous-source";
        int order = best ? version_order(row->version, best->version) : 1;
        if (order > 0) { best = row; tie = false; }
        else if (!order) tie = true;
    }
    if (tie) return "ambiguous-version";
    if (!best) {
        if (r->generate) return found ? "requirement-conflict" : "package-not-found";
        return found ? "lock-conflict" : "lock-missing";
    }
    r->chain[depth] = best->name;
    const char *deps = best->depends;
    while (*deps) {
        char dep[64];
        size_t n = strcspn(deps, ",");
        memcpy(dep, deps, n);
        dep[n] = 0;
        const char *error = resolve(r, dep, step, depth + 1);
        if (error) return error;
        deps += n;
        if (*deps) ++deps;
    }
    if (r->count == BP_MAX_STEPS) return "lock-limit";
    best->first_step = step;
    r->order[r->count++] = best;
    return NULL;
}

static const char *write_lock(FILE *file, void *context)
{
    const struct resolution *r = context;
    if (fprintf(file, "brw-lock-v1\t%s\n", r->platform) < 0) return "lock-write";
    for (size_t i = 0; i < r->count; ++i) {
        const struct bp_package *p = r->order[i];
        if (fprintf(file, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", p->registry, p->name,
                    p->version, p->kind, p->location, p->digest, p->entry, p->depends) < 0)
            return "lock-write";
    }
    return NULL;
}

static bool matches_file(FILE *file, const char *expected)
{
    SHA256Context ctx;
    if (SHA256Reset(&ctx)) return false;
    unsigned char bytes[16384], digest[SHA256HashSize];
    size_t n;
    while ((n = fread(bytes, 1, sizeof bytes, file)) != 0)
        if (SHA256Input(&ctx, bytes, (unsigned)n)) return false;
    if (ferror(file) || SHA256Result(&ctx, digest)) return false;
    char hex[65];
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    return equal(hex, expected);
}

static char *cache_path(const struct bp_package *p, bool create)
{
    char *dir = bp_cache_dir(create);
    char name[80];
    snprintf(name, sizeof name, "artifact-%s", p->digest);
    for (char *s = name; *s; ++s) *s = (char)tolower((unsigned char)*s);
    char *path = bp_join(dir, name);
    free(dir);
    return path;
}

static bool verified_path(const char *path, const struct bp_package *p)
{
    FILE *file = path ? bp_open_read(path) : NULL;
    if (!file) return false;
    bool match = matches_file(file, p->digest);
    fclose(file);
    return match;
}

static bool cached(const struct bp_package *p)
{
    char *path = cache_path(p, false);
    bool match = verified_path(path, p);
    free(path);
    return match;
}

/* Only a single-file package can be verified from its installed entry.
   An extracted archive cannot reproduce the digest of its original bytes. */
static char *installed_artifact(const struct bp_package *p)
{
    if (strcmp(p->kind, "exe")) return NULL;
    char relative[512];
    int n = snprintf(relative, sizeof relative, "packages/%s/%s/%s", p->name, p->version, p->entry);
    if (n < 0 || (size_t)n >= sizeof relative) return NULL;
    char *data = bp_data_dir();
    char *path = bp_join(data, relative);
    free(data);
    if (!verified_path(path, p)) { free(path); return NULL; }
    return path;
}

struct fetch {
    const struct bp_package *p;
    char *location;
};

static const char *locate(const char *source, const char *catalog, void *context)
{
    struct fetch *f = context;
    if (!equal(source, f->p->registry)) return NULL;
    if (f->location) return "ambiguous-source";
    if (!bp_package_id(f->p->location)) {
        f->location = copy(f->p->location);
    } else {
        char *base = copy(catalog);
        if (!base) return "out-of-memory";
        char *end = strrchr(base, '/');
        if (!end) { free(base); return "catalog-path"; }
        *end = 0;
        f->location = bp_join(base, f->p->location);
        free(base);
    }
    return f->location ? NULL : "out-of-memory";
}

static const char *fetch_verified(FILE *file, void *context)
{
    struct fetch *f = context;
    const char *error = bp_fetch_artifact(f->location, file);
    if (!error && (fflush(file) || fseek(file, 0, SEEK_SET) || !matches_file(file, f->p->digest)))
        error = "digest-mismatch";
    return error;
}

const char *bp_apply_packages(struct bp_lock *lock, size_t step)
{
    if (!lock) return "unresolved-tool";
    for (size_t i = 0; i < lock->count; ++i) {
        struct bp_package *p = &lock->rows[i];
        if (p->first_step != step) continue;
        p->reused = cached(p);
        if (p->reused) continue;
        struct fetch f = {p, installed_artifact(p)};
        p->reused = f.location != NULL;
        const char *error = f.location ? NULL : bp_catalogs(locate, &f);
        if (!error && !f.location) error = "source-not-found";
        char *path = NULL;
        if (!error) {
            path = cache_path(p, true);
            error = path ? bp_atomic_write(path, fetch_verified, &f) : "cache-directory";
        }
        free(path);
        free(f.location);
        if (error) return error;
    }
    return NULL;
}

const char *bp_prepare_lock(const struct bp_plan *plan, const struct bp_bundle *bundle,
                            const char *directory, const char *target, bool generate,
                            struct bp_lock *lock, char detail[BP_DETAIL_CAP])
{
    size_t first = 0;
    while (first < plan->count && plan->steps[first].op != BP_TOOL) ++first;
    if (first == plan->count) return NULL;
    struct resolution r = {.plan = plan, .platform = target, .detail = detail, .generate = generate};
    char name[128], header[128];
    snprintf(name, sizeof name, "blueprint.%s.lock", target);
    snprintf(header, sizeof header, "brw-lock-v1\t%s", target);
    snprintf(detail, BP_DETAIL_CAP, "%s (%s)", plan->steps[first].name, target);
    for (size_t i = 0; i < bundle->count; ++i)
        if (equal(bundle->files[i].path, name)) return "reserved-lock-path";
    char *path = bp_join(directory, name);
    if (!path) return "out-of-memory";
    const char *error = NULL;
    if (generate) error = bp_catalogs(load_catalog, &r.candidates);
    else {
        FILE *file = bp_open_read(path);
        error = file ? read_rows(&r.candidates, file, header, NULL) : "lock-missing";
        if (file) fclose(file);
    }
    for (size_t i = first; !error && i < plan->count; ++i) {
        const struct bp_step *s = &plan->steps[i];
        if (s->op != BP_TOOL) continue;
        snprintf(detail, BP_DETAIL_CAP, "%s (%s)", s->name, target);
        if (!bp_package_id(s->name) || (s->registry && !bp_package_id(s->registry)) ||
            !valid_requirement(s->version)) error = "invalid-tool";
        else error = resolve(&r, s->name, i, 0);
    }
    if (!error) {
        lock->rows = calloc(r.count, sizeof *lock->rows);
        if (!lock->rows) error = "out-of-memory";
    }
    if (!error && generate) error = bp_atomic_write(path, write_lock, &r);
    if (!error) {
        for (size_t i = 0; i < r.count; ++i) {
            lock->rows[i] = *r.order[i];
            r.order[i]->storage = NULL;
            struct bp_package *p = &lock->rows[i];
            p->reused = cached(p);
            if (!p->reused) {
                char *installed = installed_artifact(p);
                p->reused = installed != NULL;
                free(installed);
            }
        }
        lock->count = r.count;
    }
    for (size_t i = 0; i < r.candidates.count; ++i) free(r.candidates.rows[i].storage);
    free(r.candidates.rows);
    free(path);
    return error;
}

void bp_lock_free(struct bp_lock *lock)
{
    for (size_t i = 0; i < lock->count; ++i) free(lock->rows[i].storage);
    free(lock->rows);
    *lock = (struct bp_lock){0};
}
