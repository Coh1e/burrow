#include "verified_file.h"
#include "sha.h"
#include <limits.h>
#include <string.h>

static unsigned lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool brw_same_ascii(const char *a, const char *b, size_t n)
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
        return brw_same_ascii(part, "con", 3) || brw_same_ascii(part, "prn", 3) ||
               brw_same_ascii(part, "aux", 3) || brw_same_ascii(part, "nul", 3);
    return stem == 4 && part[3] >= '0' && part[3] <= '9' &&
           (brw_same_ascii(part, "com", 3) || brw_same_ascii(part, "lpt", 3));
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

bool brw_valid_sha256(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        unsigned c = lower((unsigned char)s[i]);
        if (!(c >= 'a' && c <= 'f') && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

bool brw_sha256_matches(const unsigned char *data, size_t size, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t hash[SHA256HashSize];
    SHA256Context ctx;
    if (!data || size > UINT_MAX || !brw_valid_sha256(expected)) return false;
    if (SHA256Reset(&ctx) || SHA256Input(&ctx, data, (unsigned int)size) ||
        SHA256Result(&ctx, hash)) return false;
    for (size_t i = 0; i < sizeof hash; ++i)
        if (lower((unsigned char)expected[2*i]) != (unsigned)hex[hash[i] >> 4] ||
            lower((unsigned char)expected[2*i+1]) != (unsigned)hex[hash[i] & 15]) return false;
    return true;
}
