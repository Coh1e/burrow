#include "blueprint.h"
#include "blueprint_bundle_io.h"
#include "sha.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MANIFEST_LIMIT = 128 * 1024, MODULE_LIMIT = 1024 * 1024,
       PAYLOAD_LIMIT = 64 * 1024 * 1024, BUNDLE_LIMIT = 128 * 1024 * 1024 };

static unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool same_ascii(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return false;
    return true;
}

static bool reserved(const char *part, size_t n)
{
    size_t stem = 0;
    while (stem < n && part[stem] != '.') ++stem;
    while (stem && part[stem - 1] == ' ') --stem;
    if (stem == 3)
        return same_ascii(part, "con", 3) || same_ascii(part, "prn", 3) ||
               same_ascii(part, "aux", 3) || same_ascii(part, "nul", 3);
    return stem == 4 && part[3] >= '0' && part[3] <= '9' &&
           (same_ascii(part, "com", 3) || same_ascii(part, "lpt", 3));
}

static bool safe_relative_path(const char *path, size_t max_length)
{
    if (!path || !*path || strlen(path) > max_length) return false;
    const char *part = path;
    for (const char *p = path; ; ++p) {
        unsigned c = (unsigned char)*p;
        if (!c || c == '/') {
            if (p == part || p[-1] == '.' || p[-1] == ' ' || reserved(part, (size_t)(p - part)))
                return false;
            if (!c) return true;
            part = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' '))
            return false;
    }
}

bool bp_safe_path(const char *path)
{
    return safe_relative_path(path, 255);
}

bool bp_safe_package_path(const char *path)
{
    return safe_relative_path(path, BP_PACKAGE_PATH_CAP - 1);
}

static bool valid_digest(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        unsigned c = lower((unsigned char)s[i]);
        if (!(c >= 'a' && c <= 'f') && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

static bool matches(const unsigned char *data, size_t size, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t hash[SHA256HashSize];
    SHA256Context ctx;
    if (SHA256Reset(&ctx) || SHA256Input(&ctx, data, (unsigned int)size) ||
        SHA256Result(&ctx, hash)) return false;
    for (size_t i = 0; i < sizeof hash; ++i)
        if (lower((unsigned char)expected[2*i]) != (unsigned)hex[hash[i] >> 4] ||
            lower((unsigned char)expected[2*i+1]) != (unsigned)hex[hash[i] & 15]) return false;
    return true;
}

static char *copy_string(const char *s)
{
    size_t len = strlen(s) + 1;
    char *out = malloc(len);
    if (out) memcpy(out, s, len);
    return out;
}

const struct bp_file *bp_find(const struct bp_bundle *bundle, const char *path)
{
    if (!bundle || !path) return NULL;
    for (size_t i = 0; i < bundle->count; ++i)
        if (!strcmp(bundle->files[i].path, path)) return &bundle->files[i];
    return NULL;
}

static const char *add_file(struct bp_bundle *bundle, const char *path, const char *digest, bool module)
{
    if (!bp_safe_path(path)) return "unsafe-bundle-path";
    if (!valid_digest(digest)) return "invalid-digest";
    size_t n = strlen(path);
    if (n == strlen("blueprint.manifest") && same_ascii(path, "blueprint.manifest", n))
        return "duplicate-bundle-path";
    for (size_t i = 0; i < bundle->count; ++i) {
        const char *prev = bundle->files[i].path;
        if (strlen(prev) == n && same_ascii(prev, path, n)) return "duplicate-bundle-path";
    }
    if (bundle->count == BP_MAX_FILES) return "bundle-limit";
    struct bp_file *file = &bundle->files[bundle->count];
    file->path = copy_string(path);
    if (!file->path) return "out-of-memory";
    for (size_t i = 0; i < 64; ++i) file->digest[i] = (char)lower((unsigned char)digest[i]);
    file->digest[64] = 0;
    file->module = module;
    ++bundle->count;
    return NULL;
}

static char *take_field(char **rest, char separator)
{
    char *field = *rest;
    char *end = strchr(field, separator);
    if (end) *end++ = 0;
    *rest = end;
    return field;
}

/* All v1 field names, targets, capability names, package-relative paths and
   hex digests are ASCII. Reject invalid bytes before exposing diagnostics. */
static bool valid_manifest_bytes(const unsigned char *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        unsigned c = data[i];
        if (c >= 32 && c <= 126) continue;
        if (c == '\n' || c == '\t' || (c == '\r' && i + 1 < size && data[i+1] == '\n')) continue;
        return false;
    }
    return true;
}

static const char *parse_manifest(char *text, const char *platform, const char *arch,
                                  unsigned capabilities, struct bp_bundle *bundle, char *detail)
{
    bool header = false, got_platform = false, got_arch = false;
    while (text && *text) {
        char *line = take_field(&text, '\n');
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!header) {
            if (strcmp(line, "brw-blueprint-v1")) return "unsupported-manifest";
            header = true;
            continue;
        }
        char *kind = take_field(&line, '\t');
        if (!line || !*line) return "invalid-manifest";
        char *value = take_field(&line, '\t');
        snprintf(detail, BP_DETAIL_CAP, "%s", value);
        if (!strcmp(kind, "module") || !strcmp(kind, "payload")) {
            const char *error = add_file(bundle, value, line, !strcmp(kind, "module"));
            if (error) return error;
        } else if (line) return "invalid-manifest";
        else if (!strcmp(kind, "entry")) {
            if (bundle->entry) return "invalid-manifest";
            if (!bp_safe_path(value)) return "unsafe-bundle-path";
            bundle->entry = copy_string(value);
            if (!bundle->entry) return "out-of-memory";
        } else if (!strcmp(kind, "requires") || !strcmp(kind, "optional")) {
            enum bp_op op = BP_COPY;
            while (op < BP_OP_COUNT && strcmp(bp_op_name(op), value)) ++op;
            if (op == BP_OP_COUNT) return "missing-capability";
            if (!(capabilities & (1u << op))) {
                if (!strcmp(kind, "requires")) return "missing-capability";
                bundle->unavailable |= 1u << op;
            }
            if (bundle->required & (1u << op)) return "invalid-manifest";
            bundle->required |= 1u << op;
        } else if (!strcmp(kind, "platform")) {
            if (got_platform) return "invalid-manifest";
            if (strcmp(value, platform) && strcmp(value, "any")) return "unsupported-platform";
            got_platform = true;
        } else if (!strcmp(kind, "arch")) {
            if (got_arch) return "invalid-manifest";
            if (strcmp(value, arch) && strcmp(value, "any")) return "unsupported-architecture";
            got_arch = true;
        } else return "invalid-manifest";
    }
    if (!header || !got_arch || !got_platform || !bundle->entry) return "invalid-manifest";
    const struct bp_file *entry = bp_find(bundle, bundle->entry);
    if (!entry || !entry->module) {
        snprintf(detail, BP_DETAIL_CAP, "%s", bundle->entry);
        return "missing-entry-module";
    }
    return NULL;
}

const char *bp_load(const char *directory, const char *digest, const char *platform,
                    const char *arch, unsigned capabilities, struct bp_bundle *out,
                    char detail[BP_DETAIL_CAP])
{
    if (!directory || !*directory || !platform || !arch || !out || !detail) return "invalid-bundle";
    snprintf(out->platform, sizeof out->platform, "%s", platform);
    snprintf(detail, BP_DETAIL_CAP, "blueprint.manifest");
    if (!valid_digest(digest)) return "invalid-digest";
    struct bp_directory *dir = bp_directory_open(directory);
    if (!dir) return "bundle-unavailable";
    unsigned char *manifest = NULL;
    size_t size = 0;
    const char *error = bp_directory_read(dir, "blueprint.manifest", MANIFEST_LIMIT, &manifest, &size);
    if (!error && !matches(manifest, size, digest)) error = "manifest-digest-mismatch";
    if (!error && !valid_manifest_bytes(manifest, size)) error = "invalid-manifest";
    if (!error) error = parse_manifest((char *)manifest, platform, arch, capabilities, out, detail);
    free(manifest);
    size_t remaining = BUNDLE_LIMIT;
    for (size_t i = 0; !error && i < out->count; ++i) {
        struct bp_file *file = &out->files[i];
        snprintf(detail, BP_DETAIL_CAP, "%s", file->path);
        size_t limit = file->module ? MODULE_LIMIT : PAYLOAD_LIMIT;
        if (limit > remaining) limit = remaining;
        error = bp_directory_read(dir, file->path, limit, &file->data, &file->size);
        if (!error && !matches(file->data, file->size, file->digest)) error = "file-digest-mismatch";
        if (!error && file->module && memchr(file->data, 0, file->size)) error = "invalid-module";
        remaining -= file->size;
    }
    bp_directory_close(dir);
    if (!error) detail[0] = 0;
    return error;
}

void bp_bundle_free(struct bp_bundle *bundle)
{
    if (!bundle) return;
    for (size_t i = 0; i < bundle->count; ++i) {
        free(bundle->files[i].path);
        free(bundle->files[i].data);
    }
    free(bundle->entry);
    memset(bundle, 0, sizeof *bundle);
}
