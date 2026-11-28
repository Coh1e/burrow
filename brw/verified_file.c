#include "verified_file.h"
#include "sha.h"
#include <limits.h>
#include <string.h>

bool bp_valid_utf8(const char *text, size_t size)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + size;
    while (p < end) {
        unsigned lead = *p++;
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        unsigned n = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if ((size_t)(end - p) < n || (lead == 0xe0 && *p < 0xa0) ||
            (lead == 0xed && *p >= 0xa0) || (lead == 0xf0 && *p < 0x90) ||
            (lead == 0xf4 && *p >= 0x90)) return false;
        while (n--) if ((*p++ & 0xc0) != 0x80) return false;
    }
    return true;
}

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

/* Validate the network-independent parts on both preview and apply. Host
   reachability, response length and certificate checks belong to transport. */
bool brw_valid_http_url(const char *url)
{
    if (!url || !bp_valid_utf8(url, strlen(url))) return false;
    size_t scheme = strcspn(url, ":");
    if (!((scheme == 4 && brw_same_ascii(url, "http", 4)) ||
          (scheme == 5 && brw_same_ascii(url, "https", 5))) ||
        strncmp(url + scheme, "://", 3)) return false;
    const char *host = url + scheme + 3;
    size_t authority = strcspn(host, "/?");
    if (!authority || memchr(host, '@', authority)) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
        if (*p <= ' ' || *p == 127 || *p == '\\' || *p == '#') return false;

    const char *end = host + authority;
    const char *port;
    if (host[0] == '[') {
        const char *close = memchr(host + 1, ']', authority - 1);
        /* Longest numeric IPv6 text is 45 bytes, including a dotted IPv4 tail. */
        char address[46];
        if (!close || (size_t)(close - host - 1) >= sizeof address) return false;
        size_t length = (size_t)(close - host - 1);
        memcpy(address, host + 1, length);
        address[length] = 0;
        if (!brw_valid_ipv6_literal(address)) return false;
        port = close + 1;
    } else {
        port = host + strcspn(host, ":/?");
        if (port == host || memchr(host, '[', authority) || memchr(host, ']', authority)) return false;
    }
    if (port == end) return true;
    if (*port++ != ':' || port == end) return false;
    unsigned number = 0;
    for (; port < end; ++port) {
        if (*port < '0' || *port > '9') return false;
        number = number * 10u + (unsigned)(*port - '0');
        if (number > 65535) return false;
    }
    return number != 0;
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

bool brw_sha256_digest_matches(const unsigned char hash[32], const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    if (!hash || !brw_valid_sha256(expected)) return false;
    for (size_t i = 0; i < SHA256HashSize; ++i)
        if (lower((unsigned char)expected[2*i]) != (unsigned)hex[hash[i] >> 4] ||
            lower((unsigned char)expected[2*i+1]) != (unsigned)hex[hash[i] & 15]) return false;
    return true;
}

bool brw_sha256_matches(const unsigned char *data, size_t size, const char *expected)
{
    uint8_t hash[SHA256HashSize];
    SHA256Context ctx;
    return data && size <= UINT_MAX && !SHA256Reset(&ctx) &&
           !SHA256Input(&ctx, data, (unsigned int)size) && !SHA256Result(&ctx, hash) &&
           brw_sha256_digest_matches(hash, expected);
}
