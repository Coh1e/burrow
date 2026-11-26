#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include "blueprint_lock.h"
#include "install_windows.h"
#include "paths_windows.h"
#include "source_windows.h"

static wchar_t *wide_path(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *result = n ? malloc((size_t)n * sizeof *result) : NULL;
    if (result) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, result, n);
    return result;
}

static char *utf8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, NULL, 0, NULL, NULL);
    char *p = n ? malloc((size_t)n) : NULL;
    if (p) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, p, n, NULL, NULL);
    return p;
}

struct visitor { bp_catalog_visit visit; void *context; };

static const char *visit(const wchar_t *name, const wchar_t *catalog, void *context)
{
    struct visitor *v = context;
    char *source = utf8(name), *path = utf8(catalog);
    if (path)
        for (char *p = path; *p; ++p)
            if (*p == '\\') *p = '/';
    const char *error = source && path ? v->visit(source, path, v->context) : "catalog-path";
    free(path);
    free(source);
    return error;
}

const char *bp_catalogs(bp_catalog_visit callback, void *context)
{
    struct visitor v = {callback, context};
    return brw_visit_catalogs(visit, &v);
}

char *bp_cache_dir(bool create)
{
    wchar_t path[BRW_PATH_CAP];
    return brw_path(BRW_CACHE, path, create) ? NULL : utf8(path);
}

char *bp_data_dir(void)
{
    wchar_t path[BRW_PATH_CAP];
    return brw_path(BRW_DATA, path, false) ? NULL : utf8(path);
}

char *bp_package_child(const char *parent, const char *name)
{
    return bp_join(parent, name);
}

static FILE *stream(HANDLE h, int flags, const char *mode)
{
    if (h == INVALID_HANDLE_VALUE) return NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT | flags);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE *f = _fdopen(fd, mode);
    if (!f) _close(fd);
    return f;
}

FILE *bp_open_read(const char *path)
{
    wchar_t *w = wide_path(path);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h);
        return NULL;
    }
    return stream(h, _O_RDONLY, "rb");
}

const char *bp_atomic_write(const char *path, bp_write_file write, void *context)
{
    wchar_t *dest = wide_path(path);
    ULONGLONG id;
    if (!dest || BCryptGenRandom(NULL, (PUCHAR)&id, sizeof id, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        free(dest);
        return "stage-create";
    }
    size_t n = wcslen(dest) + 24;
    wchar_t *stage = malloc(n * sizeof *stage);
    if (!stage) { free(dest); return "out-of-memory"; }
    swprintf(stage, n, L"%ls.%016llx.tmp", dest, id);
    HANDLE h = CreateFileW(stage, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    const char *error = "stage-create";
    if (h != INVALID_HANDLE_VALUE) {
        FILE *f = stream(h, _O_RDWR, "w+b");
        if (f) {
            error = write(f, context);
            if (fclose(f) && !error) error = "stage-close";
            if (!error && !MoveFileExW(stage, dest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                error = "stage-publish";
        }
        if (error) DeleteFileW(stage);
    }
    free(stage);
    free(dest);
    return error;
}

const char *bp_fetch_artifact(const char *location, FILE *file)
{
    wchar_t *path = wide_path(location);
    const char *error = path ? brw_download(path, file) : "invalid-location";
    free(path);
    return error;
}

const char bp_home_key[] = "USERPROFILE";

bool bp_env_key_equal(const char *name, const char *key)
{
    return _stricmp(name, key) == 0;
}

char *bp_default_entry(const struct bp_package *p, const char *bin_override, const char *home_override)
{
    wchar_t bin[BRW_PATH_CAP], path[BRW_PATH_CAP];
    wchar_t *package = wide_path(p->name);
    wchar_t *planned_bin = bin_override ? wide_path(bin_override) : NULL;
    wchar_t *planned_home = home_override ? wide_path(home_override) : NULL;
    char *result = package && (!bin_override || planned_bin) && (!home_override || planned_home) &&
        !brw_planned_bin(bin, planned_bin, planned_home) &&
        brw_command_entry_path(bin, package, path) ? utf8(path) : NULL;
    free(planned_bin);
    free(planned_home);
    free(package);
    return result;
}

const char *bp_default_matches(const struct bp_package *p, bool *matches)
{
    wchar_t *name = wide_path(p->name), *version = wide_path(p->version), *entry = wide_path(p->entry);
    const char *error = brw_default_matches(name, version, entry, matches);
    free(name);
    free(version);
    free(entry);
    return error;
}

static const char *use_package(const struct bp_package *p, const char *cache, char **program, bool *changed)
{
    wchar_t *name = wide_path(p->name), *version = wide_path(p->version), *path = wide_path(cache);
    wchar_t *digest = wide_path(p->digest), *entry = wide_path(p->entry), *depends = wide_path(p->depends);
    struct local_package pkg = {name, version, path, digest, entry};
    wchar_t target[BRW_PATH_CAP];
    const char *error = name && version && path && digest && entry && depends ?
        brw_use_locked_package(&pkg, !strcmp(p->kind, "exe"), depends, changed, target) : "invalid-package";
    if (!error && program && !(*program = utf8(target))) error = "out-of-memory";
    free(name);
    free(version);
    free(path);
    free(digest);
    free(entry);
    free(depends);
    return error;
}

const char *bp_materialize_tool(const struct bp_package *p, const char *cache, char **program)
{
    return use_package(p, cache, program, NULL);
}

const char *bp_set_default(const struct bp_package *p, const char *cache, bool *changed)
{
    return use_package(p, cache, NULL, changed);
}
