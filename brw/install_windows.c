#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <archive.h>
#include <archive_entry.h>
#include <stdbool.h>
#include <stdio.h>
#include <share.h>
#include <string.h>
#include "install_windows.h"
#include "source_windows.h"
#include "paths_windows.h"
#include "init_windows.h"
#include "shim_image.h"
#include "shim_windows.h"

enum { PATH_CAP = BRW_PATH_CAP, NAME_CAP = 128, ENTRY_CAP = MAX_PATH, ARCHIVE_DEPTH = 16 };

struct installation {
    const struct local_package *package;
    const wchar_t *dependencies;
    enum digby_action action;
    FILE *archive;
    bool source;
    bool executable;
    wchar_t download_stage[PATH_CAP];
    HANDLE lock;
    wchar_t packages[PATH_CAP];
    wchar_t state[PATH_CAP];
    wchar_t package_dir[PATH_CAP];
    wchar_t version[PATH_CAP];
    wchar_t bin[PATH_CAP];
    wchar_t shim[PATH_CAP];
    wchar_t sidecar[PATH_CAP];
    wchar_t stage[PATH_CAP];
    wchar_t active_version[NAME_CAP];
    wchar_t active_entry[ENTRY_CAP];
    wchar_t shim_hash[65];
};

static bool join(wchar_t path[PATH_CAP], const wchar_t *base, const wchar_t *name)
{
    if (wcslen(base) + wcslen(name) + 2 > PATH_CAP)
        return false;
    wcscpy(path, base);
    wcscat(path, L"\\");
    wcscat(path, name);
    return true;
}

/* Package path components use ASCII names and exclude Windows device names. */
bool brw_package_component(const wchar_t *name)
{
    size_t length = wcslen(name);
    if (!length || length >= NAME_CAP || *name == L'.' || name[length - 1] == L'.')
        return false;
    for (size_t i = 0; i < length; ++i) {
        wchar_t c = name[i];
        if (!(c >= L'a' && c <= L'z') && !(c >= L'A' && c <= L'Z') &&
            !(c >= L'0' && c <= L'9') && c != L'.' && c != L'-' && c != L'_')
            return false;
    }
    size_t stem = wcscspn(name, L".");
    if (stem == 3 && (!_wcsnicmp(name, L"con", 3) || !_wcsnicmp(name, L"prn", 3) ||
                      !_wcsnicmp(name, L"aux", 3) || !_wcsnicmp(name, L"nul", 3)))
        return false;
    return !(stem == 4 && name[3] >= L'0' && name[3] <= L'9' &&
             (!_wcsnicmp(name, L"com", 3) || !_wcsnicmp(name, L"lpt", 3)));
}

/* Canonical paths use forward slashes and have no empty or special components. */
static bool relative_path(const wchar_t *path)
{
    if (!*path || wcslen(path) >= ENTRY_CAP)
        return false;
    unsigned depth = 0;
    for (const wchar_t *part = path; *part;) {
        wchar_t component[NAME_CAP];
        size_t length = wcscspn(part, L"/");
        if (!length || length >= NAME_CAP || ++depth > ARCHIVE_DEPTH)
            return false;
        wmemcpy(component, part, length);
        component[length] = 0;
        if (!brw_package_component(component))
            return false;
        if (!part[length])
            return true;
        part += length + 1;
    }
    return false;
}

static const char *validate_package(const struct local_package *package, bool nested)
{
    if (!brw_package_component(package->name) || !brw_package_component(package->version) ||
        !(nested ? relative_path(package->entry) : brw_package_component(package->entry)))
        return "invalid-package";
    const wchar_t *filename = wcsrchr(package->entry, L'/');
    filename = filename ? filename + 1 : package->entry;
    size_t length = wcslen(filename);
    const wchar_t *ext = wcsrchr(filename, L'.');
    if (length <= 4 || !ext || (_wcsicmp(ext, L".exe") && _wcsicmp(ext, L".cmd") &&
                                 _wcsicmp(ext, L".bat") && _wcsicmp(ext, L".ps1")))
        return "invalid-entry";
    if (!*package->archive || wcslen(package->digest) != 64)
        return "invalid-input";
    for (size_t i = 0; i < 64; ++i) {
        if (!wcschr(L"0123456789abcdefABCDEF", package->digest[i]))
            return "invalid-digest";
    }
    return NULL;
}

const char *brw_validate_package(const struct local_package *package)
{
    return validate_package(package, false);
}

const char *brw_validate_source_package(const struct local_package *package, bool executable)
{
    return validate_package(package, !executable);
}

const char *brw_validate_lifecycle(enum digby_action action, const struct local_package *package)
{
    if (action == DIGBY_INSTALL || action == DIGBY_UPDATE)
        return brw_validate_package(package);
    if (!brw_package_component(package->name) || (action == DIGBY_ROLLBACK && !brw_package_component(package->version)))
        return "invalid-package";
    return NULL;
}

enum { RUNNING, CANCELLED, PUBLISHING };
static volatile LONG publish_state;

bool brw_cancelled(void)
{
    return (InterlockedCompareExchange(&publish_state, RUNNING, RUNNING) & CANCELLED) != 0;
}

static BOOL WINAPI cancel_install(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedOr(&publish_state, CANCELLED);
    return TRUE;
}

bool brw_begin_source_install(void)
{
    InterlockedExchange(&publish_state, RUNNING);
    return SetConsoleCtrlHandler(cancel_install, TRUE) != 0;
}

void brw_end_source_install(void)
{
    SetConsoleCtrlHandler(cancel_install, FALSE);
}

static bool hash_contents(FILE *file, const unsigned char *image, DWORD length, wchar_t hex[65])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool success = false;
    unsigned char bytes[8192];
    unsigned char digest[32];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0)
        goto done;
    if (file) {
        size_t count;
        while ((count = fread(bytes, 1, sizeof bytes, file)) != 0) {
            if (brw_cancelled() || BCryptHashData(hash, bytes, (ULONG)count, 0) < 0)
                goto done;
        }
        if (ferror(file))
            goto done;
    } else if (BCryptHashData(hash, (PUCHAR)image, length, 0) < 0)
        goto done;
    if (BCryptFinishHash(hash, digest, sizeof digest, 0) < 0)
        goto done;
    for (size_t i = 0; i < sizeof digest; ++i) {
        const wchar_t *digits = L"0123456789abcdef";
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    success = true;

done:
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

static const char *verify(struct installation *install)
{
    if (!install->archive)
        install->archive = _wfsopen(install->package->archive, L"rb", _SH_DENYWR);
    if (!install->archive)
        return "archive-open";
    wchar_t digest[65];
    if (!hash_contents(install->archive, NULL, 0, digest) ||
        fseek(install->archive, 0, SEEK_SET) != 0)
        return brw_cancelled() ? "cancelled" : "digest-read";
    return _wcsicmp(install->package->digest, digest) ? "digest-mismatch" : NULL;
}

static bool directory(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL))
        return true;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static const char *package_roots(struct installation *install, bool create)
{
    wchar_t data[PATH_CAP];
    const char *error = brw_path(BRW_BIN, install->bin, false);
    if (!error)
        error = brw_path(BRW_DATA, data, create);
    if (error)
        return error;
    if (!join(install->packages, data, L"packages") ||
        (create && !directory(install->packages)))
        return "data-directory";
    return NULL;
}

static bool package_paths(struct installation *install)
{
    wchar_t alias[NAME_CAP + 12];
    swprintf(alias, NAME_CAP + 12, L"%ls.exe", install->package->name);
    if (!join(install->package_dir, install->packages, install->package->name) ||
        (install->package->version &&
         !join(install->version, install->package_dir, install->package->version)) ||
        !join(install->shim, install->bin, alias))
        return false;
    wcscpy(install->sidecar, install->shim);
    return brw_append_shim_suffix(install->sidecar, wcslen(install->shim));
}

bool brw_next_dependency(const wchar_t **names, wchar_t name[BRW_PACKAGE_NAME_CAP])
{
    size_t length = wcscspn(*names, L",");
    if (!length || length >= BRW_PACKAGE_NAME_CAP)
        return false;
    wmemcpy(name, *names, length);
    name[length] = 0;
    *names += length;
    if (**names == L',') {
        ++*names;
        if (!**names)
            return false;
    }
    return brw_package_component(name);
}

bool brw_dependency_names(const wchar_t *names)
{
    if (!names)
        return true;
    if (wcslen(names) >= BRW_DEPENDENCIES_CAP)
        return false;
    while (*names) {
        wchar_t name[BRW_PACKAGE_NAME_CAP];
        if (!brw_next_dependency(&names, name))
            return false;
    }
    return true;
}

static HANDLE package_changes;

const char *brw_begin_package_changes(void)
{
    package_changes = brw_user_mutex(L"PackageMutation");
    if (!package_changes)
        return "package-busy";
    DWORD result = WaitForSingleObject(package_changes, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
        return NULL;
    CloseHandle(package_changes);
    package_changes = NULL;
    return "package-busy";
}

void brw_end_package_changes(void)
{
    ReleaseMutex(package_changes);
    CloseHandle(package_changes);
    package_changes = NULL;
}

static bool stage_directory(const wchar_t *base, wchar_t stage[PATH_CAP])
{
    wchar_t name[64];
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        swprintf(name, 64, L".stage-%lu-%llu-%u", GetCurrentProcessId(),
                 GetTickCount64(), attempt);
        if (!join(stage, base, name))
            break;
        if (CreateDirectoryW(stage, NULL))
            return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    stage[0] = 0;
    return false;
}

static const char *download(struct installation *install)
{
    wchar_t cache[PATH_CAP], destination[PATH_CAP];
    const char *error = brw_path(BRW_CACHE, cache, true);
    if (error)
        return error;
    if (!stage_directory(cache, install->download_stage) ||
        !join(destination, install->download_stage, L"download"))
        return "download-stage";
    install->archive = _wfsopen(destination, L"w+b", _SH_DENYWR);
    if (!install->archive)
        return "download-stage";
    if (setvbuf(install->archive, NULL, _IONBF, 0))
        return "download-stage";
    return brw_download(install->package->archive, install->archive);
}

static bool ordinary_path(const wchar_t *path, bool expect_directory)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !!(attributes & FILE_ATTRIBUTE_DIRECTORY) == expect_directory;
}

static bool ordinary_payload(const wchar_t *version, const wchar_t *entry)
{
    wchar_t path[PATH_CAP];
    if (!relative_path(entry) || !join(path, version, entry))
        return false;
    for (wchar_t *slash = wcschr(path + wcslen(version) + 1, L'/'); slash;
         slash = wcschr(slash + 1, L'/')) {
        *slash = 0;
        bool ordinary = ordinary_path(path, true);
        *slash = L'/';
        if (!ordinary)
            return false;
    }
    return ordinary_path(path, false);
}

static bool read_record(const wchar_t *path, wchar_t *value, size_t capacity)
{
    if (!ordinary_path(path, false))
        return false;
    FILE *file = _wfsopen(path, L"rb", _SH_DENYWR);
    if (!file)
        return false;
    size_t bytes = fread(value, 1, capacity * sizeof *value, file);
    size_t length = bytes / sizeof *value;
    bool valid = fgetc(file) == EOF && !ferror(file) && bytes % sizeof *value == 0 && length > 1 &&
                 value[length - 1] == 0 && wcslen(value) == length - 1;
    fclose(file);
    return valid;
}

static bool owned_shim(struct installation *install, const wchar_t *version)
{
    wchar_t record[PATH_CAP], expected[65];
    if (!ordinary_path(install->shim, false) || !join(record, version, L".brw-shim"))
        return false;
    FILE *file = _wfsopen(install->shim, L"rb", _SH_DENYWR);
    if (!file)
        return false;
    bool hashed = hash_contents(file, NULL, 0, install->shim_hash);
    fclose(file);
    if (!hashed)
        return false;
    if (!read_record(record, expected, 65)) {
        /* #120 installations predate digest records. Accept the exact launcher
           shipped in that Windows build, or the current console image. */
        if (GetFileAttributesW(record) == INVALID_FILE_ATTRIBUTES &&
            !wcscmp(install->shim_hash, L"5ab9111d4d47ddda94b7eab84e216d67027d25b14bd66378e1b11e480de76c38"))
            return true;
        if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES ||
            !hash_contents(NULL, shim_image, sizeof shim_image, expected))
            return false;
    }
    return !wcscmp(install->shim_hash, expected);
}

static const char *read_active(struct installation *install);

static const char *prepare(struct installation *install)
{
    const char *error = package_roots(install, false);
    if (!error)
        error = brw_writable_path(BRW_STATE, install->state);
    if (error)
        return error;
    if (!package_paths(install))
        return "install-directory";
    if (install->action == DIGBY_INSTALL && (error = package_roots(install, true)))
        return error;
    if (!ordinary_path(install->packages, true))
        return install->action == DIGBY_INSTALL ? "install-directory" : "not-installed";
    error = brw_path(BRW_STATE, install->state, true);
    if (error)
        return error;
    wchar_t name[NAME_CAP + 8], lock[PATH_CAP];
    swprintf(name, NAME_CAP + 8, L".lock-%ls", install->package->name);
    if (!join(lock, install->packages, name))
        return "install-directory";
    install->lock = CreateFileW(lock, DELETE | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (install->lock == INVALID_HANDLE_VALUE)
        return "package-busy";
    if ((install->action == DIGBY_INSTALL && !directory(install->package_dir)) ||
        !ordinary_path(install->package_dir, true))
        return install->action == DIGBY_INSTALL ? "install-directory" : "not-installed";
    if (install->action == DIGBY_INSTALL)
        return NULL;

    return read_active(install);
}

static const char *read_active(struct installation *install)
{
    wchar_t target[PATH_CAP];
    if (!ordinary_path(install->bin, true) || !read_record(install->sidecar, target, PATH_CAP))
        return "entry-conflict";
    size_t prefix = wcslen(install->package_dir);
    if (_wcsnicmp(target, install->package_dir, prefix) || target[prefix] != L'\\')
        return "entry-conflict";
    const wchar_t *relative = target + prefix + 1;
    const wchar_t *separator = wcschr(relative, L'\\');
    if (!separator || (size_t)(separator - relative) >= NAME_CAP || !relative_path(separator + 1))
        return "entry-conflict";
    size_t length = (size_t)(separator - relative);
    wmemcpy(install->active_version, relative, length);
    install->active_version[length] = 0;
    wchar_t version[PATH_CAP];
    if (!brw_package_component(install->active_version) ||
        !join(version, install->package_dir, install->active_version) ||
        !ordinary_path(version, true) || !ordinary_payload(version, separator + 1) ||
        !owned_shim(install, version))
        return "entry-conflict";
    wcscpy(install->active_entry, separator + 1);
    wchar_t record[PATH_CAP], recorded_entry[ENTRY_CAP];
    if (!join(record, version, L".brw-entry"))
        return "entry-conflict";
    if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES &&
        (!read_record(record, recorded_entry, ENTRY_CAP) ||
         wcscmp(install->active_entry, recorded_entry)))
        return "entry-conflict";
    return NULL;
}

bool brw_is_installed(const wchar_t *name)
{
    struct local_package package = {0};
    struct installation install = {0};
    package.name = name;
    install.package = &package;
    return !package_roots(&install, false) && package_paths(&install) &&
           ordinary_path(install.package_dir, true) && !read_active(&install);
}

static bool read_dependencies(const struct installation *install, const wchar_t *version,
                              wchar_t names[BRW_DEPENDENCIES_CAP])
{
    wchar_t directory[PATH_CAP], path[PATH_CAP];
    if (!join(directory, install->package_dir, version) || !join(path, directory, L".brw-depends"))
        return false;
    names[0] = 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    return read_record(path, names, BRW_DEPENDENCIES_CAP) && brw_dependency_names(names);
}

const char *brw_check_uninstall(const wchar_t *name, wchar_t dependent[BRW_PACKAGE_NAME_CAP])
{
    struct local_package package = {0};
    struct installation install = {0};
    install.package = &package;
    dependent[0] = 0;
    wchar_t pattern[PATH_CAP];
    const char *root_error = package_roots(&install, false);
    if (root_error)
        return root_error;
    if (!join(pattern, install.packages, L"*"))
        return "dependency-state";
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND ? NULL : "dependency-state";
    }
    const char *error = NULL;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !brw_package_component(data.cFileName) || !_wcsicmp(data.cFileName, name))
            continue;
        package.name = data.cFileName;
        if (!package_paths(&install)) {
            error = "dependency-state";
            break;
        }
        if (GetFileAttributesW(install.sidecar) == INVALID_FILE_ATTRIBUTES &&
            (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
            continue;
        wchar_t names[BRW_DEPENDENCIES_CAP], dependency[BRW_PACKAGE_NAME_CAP];
        if (read_active(&install) || !read_dependencies(&install, install.active_version, names)) {
            error = "dependency-state";
            break;
        }
        const wchar_t *cursor = names;
        while (brw_next_dependency(&cursor, dependency)) {
            if (!_wcsicmp(dependency, name)) {
                wcscpy(dependent, data.cFileName);
                error = "package-required";
                break;
            }
        }
    } while (!error && FindNextFileW(search, &data));
    if (!error && GetLastError() != ERROR_NO_MORE_FILES)
        error = "dependency-state";
    FindClose(search);
    return error;
}

/* Allow the conventional ./ prefix in source archives. Use canonical names in the inventory. */
static bool archive_path(const char *input, bool folder, bool source, wchar_t path[ENTRY_CAP])
{
    if (!input)
        return false;
    if (source) {
        while (input[0] == '.' && input[1] == '/')
            input += 2;
        if (folder && (!*input || !strcmp(input, "."))) {
            path[0] = 0;
            return true;
        }
    }
    size_t length = strlen(input);
    if (folder && length && input[length - 1] == '/')
        --length;
    if (!length || length >= ENTRY_CAP)
        return false;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)input[i] > 127)
            return false;
        path[i] = (unsigned char)input[i];
    }
    path[length] = 0;
    return source ? relative_path(path) : brw_package_component(path);
}

static const char *stage_folder(const wchar_t *stage, const wchar_t *relative, FILE *inventory)
{
    wchar_t path[PATH_CAP];
    if (!join(path, stage, relative))
        return "archive-path";
    if (!CreateDirectoryW(path, NULL))
        return ordinary_path(path, true) ? NULL : "archive-output";
    return fprintf(inventory, "%ls/\n", relative) < 0 ? "version-record" : NULL;
}

/* Only newly created directories go into the inventory, including implicit parents. */
static const char *stage_parents(const wchar_t *stage, wchar_t *relative, FILE *inventory)
{
    for (wchar_t *slash = wcschr(relative, L'/'); slash; slash = wcschr(slash + 1, L'/')) {
        *slash = 0;
        const char *error = stage_folder(stage, relative, inventory);
        *slash = L'/';
        if (error)
            return error;
    }
    return NULL;
}

static const char *extract_file(struct archive *reader, const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return "archive-output";
    char buffer[8192];
    la_ssize_t count;
    bool written = true;
    while ((count = archive_read_data(reader, buffer, sizeof buffer)) > 0) {
        DWORD size;
        if (brw_cancelled() || !WriteFile(file, buffer, (DWORD)count, &size, NULL) || size != (DWORD)count) {
            written = false;
            break;
        }
    }
    if (!CloseHandle(file))
        written = false;
    return brw_cancelled() ? "cancelled" : written && count == 0 ? NULL : "archive-data";
}

static const char *stage_executable(struct installation *install, FILE *inventory)
{
    wchar_t path[PATH_CAP];
    if (!join(path, install->stage, install->package->entry))
        return "archive-path";
    FILE *output = _wfsopen(path, L"wb", _SH_DENYWR);
    if (!output)
        return "archive-output";
    char bytes[8192];
    size_t count;
    bool written = true;
    while (!brw_cancelled() && (count = fread(bytes, 1, sizeof bytes, install->archive)) != 0) {
        if (fwrite(bytes, 1, count, output) != count) {
            written = false;
            break;
        }
    }
    if (fclose(output))
        written = false;
    if (brw_cancelled())
        return "cancelled";
    if (!written || ferror(install->archive))
        return "archive-data";
    return fprintf(inventory, "%ls\n", install->package->entry) < 0 ? "version-record" : NULL;
}

static const char *extract(struct installation *install)
{
    const char *prepare_error = prepare(install);
    if (prepare_error)
        return prepare_error;
    if (GetFileAttributesW(install->version) != INVALID_FILE_ATTRIBUTES ||
        (install->action == DIGBY_INSTALL &&
         (GetFileAttributesW(install->shim) != INVALID_FILE_ATTRIBUTES ||
          GetFileAttributesW(install->sidecar) != INVALID_FILE_ATTRIBUTES)))
        return "destination-exists";
    if (!stage_directory(install->package_dir, install->stage))
        return "stage-directory";
    wchar_t inventory[PATH_CAP];
    if (!join(inventory, install->stage, L".brw-files"))
        return "version-record";
    FILE *files = _wfopen(inventory, L"wb");
    if (!files)
        return "version-record";
    if (install->executable) {
        const char *error = stage_executable(install, files);
        return fclose(files) && !error ? "version-record" : error;
    }
    struct archive *reader = archive_read_new();
    if (!reader) {
        fclose(files);
        return "archive-open";
    }
    const char *error = "archive-open";
    bool found_entry = false;
    if (archive_read_support_format_tar(reader) == ARCHIVE_OK &&
        (!install->source || (archive_read_support_format_zip(reader) == ARCHIVE_OK &&
                              archive_read_support_filter_gzip(reader) == ARCHIVE_OK)) &&
        archive_read_open_FILE(reader, install->archive) == ARCHIVE_OK) {
        struct archive_entry *entry;
        int status;
        error = NULL;
        while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
            wchar_t name[ENTRY_CAP], path[PATH_CAP];
            bool folder = archive_entry_filetype(entry) == AE_IFDIR;
            if (brw_cancelled()) {
                error = "cancelled";
                break;
            }
            if ((!install->source && folder) ||
                (!folder && archive_entry_filetype(entry) != AE_IFREG) ||
                archive_entry_hardlink(entry) || archive_entry_symlink(entry) ||
                !archive_path(archive_entry_pathname(entry), folder, install->source, name) ||
                !join(path, install->stage, name)) {
                error = "archive-path";
                break;
            }
            if (!*name)
                continue;
            error = stage_parents(install->stage, name, files);
            if (error)
                break;
            if (folder) {
                error = stage_folder(install->stage, name, files);
                if (error)
                    break;
                continue;
            }
            error = extract_file(reader, path);
            if (error)
                break;
            if (fprintf(files, "%ls\n", name) < 0) {
                error = "version-record";
                break;
            }
            if (!wcscmp(name, install->package->entry))
                found_entry = true;
        }
        if (!error && status != ARCHIVE_EOF)
            error = "archive-data";
        if (!error && !found_entry)
            error = "entry-missing";
    }
    archive_read_free(reader);
    if (fclose(files) && !error)
        error = "version-record";
    return error;
}

static bool dots(const wchar_t *name)
{
    return !wcscmp(name, L".") || !wcscmp(name, L"..");
}

static bool remove_tree(const wchar_t *stage, unsigned depth)
{
    wchar_t path[PATH_CAP];
    if (depth > ARCHIVE_DEPTH || !ordinary_path(stage, true) || !join(path, stage, L"*"))
        return false;
    bool removed = true;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(path, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (dots(entry.cFileName))
                continue;
            if (!join(path, stage, entry.cFileName)) {
                removed = false;
                continue;
            }
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!DeleteFileW(path))
                    removed = false;
            } else if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!RemoveDirectoryW(path))
                    removed = false;
            } else if (!remove_tree(path, depth + 1)) {
                removed = false;
            }
        } while (FindNextFileW(search, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            removed = false;
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        removed = false;
    }
    return removed && RemoveDirectoryW(stage);
}

static bool remove_stage(const wchar_t *stage)
{
    return remove_tree(stage, 0);
}

/* Create a complete file under a staging name before making it visible. */
static bool publish_file(const wchar_t *bin, const wchar_t *path, const void *data,
                         DWORD length, bool replace)
{
    wchar_t temporary[PATH_CAP];
    if (wcslen(bin) >= MAX_PATH - 14 || !GetTempFileNameW(bin, L"brw", 0, temporary))
        return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    bool success = false;
    if (file != INVALID_HANDLE_VALUE) {
        success = WriteFile(file, data, length, &written, NULL) && written == length;
        if (!CloseHandle(file))
            success = false;
    }
    if (success && MoveFileExW(temporary, path, replace ? MOVEFILE_REPLACE_EXISTING : 0))
        return true;
    DeleteFileW(temporary);
    return false;
}

static bool write_record(const wchar_t *directory, const wchar_t *path,
                         const wchar_t *value, bool replace)
{
    DWORD size = (DWORD)((wcslen(value) + 1) * sizeof *value);
    return publish_file(directory, path, value, size, replace);
}

/* Identify retained versions across different data roots sharing one state root. */
static bool install_record_path(const struct installation *install, const wchar_t *version,
                                wchar_t path[PATH_CAP])
{
    wchar_t identity[PATH_CAP], digest[65], name[80];
    if (!join(identity, install->package_dir, version))
        return false;
    DWORD chars = (DWORD)wcslen(identity);
    CharLowerBuffW(identity, chars);
    if (!hash_contents(NULL, (const unsigned char *)identity, chars * sizeof *identity, digest))
        return false;
    swprintf(name, 80, L"install-%ls", digest);
    return join(path, install->state, name);
}

static bool remove_install_record(const struct installation *install, const wchar_t *version)
{
    wchar_t path[PATH_CAP], recorded[PATH_CAP], expected[PATH_CAP];
    if (!install_record_path(install, version, path) ||
        !join(expected, install->package_dir, version))
        return false;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    return read_record(path, recorded, PATH_CAP) && !_wcsicmp(recorded, expected) && DeleteFileW(path);
}

static bool version_entry(const struct installation *install, const wchar_t *version,
                          wchar_t entry[ENTRY_CAP])
{
    wchar_t directory[PATH_CAP], record[PATH_CAP];
    if (!join(directory, install->package_dir, version) || !ordinary_path(directory, true) ||
        !join(record, directory, L".brw-entry"))
        return false;
    if (!read_record(record, entry, ENTRY_CAP)) {
        if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES ||
            wcscmp(version, install->active_version))
            return false;
        /* The original installer recorded its entry only in the active sidecar. */
        wcscpy(entry, install->active_entry);
    }
    return ordinary_payload(directory, entry);
}

static bool owned_version(const wchar_t *directory, const wchar_t *entry,
                          const wchar_t *detached);

struct launcher_image {
    const unsigned char *bytes;
    DWORD size;
    wchar_t digest[65];
};

static bool select_launcher(const wchar_t *directory, const wchar_t *entry,
                            struct launcher_image *image)
{
    wchar_t path[PATH_CAP];
    if (!join(path, directory, entry))
        return false;
    const wchar_t *ext = wcsrchr(entry, L'.');
    DWORD_PTR type = ext && !_wcsicmp(ext, L".exe") ?
        SHGetFileInfoW(path, 0, NULL, 0, SHGFI_EXETYPE) : 0;
    bool gui = LOWORD(type) == IMAGE_NT_SIGNATURE && HIWORD(type) != 0;
    image->bytes = gui ? gui_shim_image : shim_image;
    image->size = gui ? sizeof gui_shim_image : sizeof shim_image;
    return hash_contents(NULL, image->bytes, image->size, image->digest);
}

static const char *switch_entry(struct installation *install, const wchar_t *target,
                                const struct launcher_image *image, bool replace)
{
    wchar_t previous[PATH_CAP];
    if (replace && !read_record(install->sidecar, previous, PATH_CAP))
        return "entry-conflict";
    if (!write_record(install->bin, install->sidecar, target, replace))
        return "publish-entry";
    if ((replace && !wcscmp(install->shim_hash, image->digest)) ||
        publish_file(install->bin, install->shim, image->bytes, image->size, replace))
        return NULL;
    if (replace ? !write_record(install->bin, install->sidecar, previous, true) :
                  !DeleteFileW(install->sidecar))
        return "entry-cleanup";
    return "publish-entry";
}
static const char *publish(struct installation *install)
{
    wchar_t target[PATH_CAP], record[PATH_CAP];
    if (!join(target, install->version, install->package->entry) ||
        brw_path(BRW_BIN, install->bin, true))
        return "entry-directory";
    if (!join(record, install->stage, L".brw-entry") ||
        !write_record(install->stage, record, install->package->entry, false))
        return "version-record";
    bool update = install->action == DIGBY_UPDATE;
    wchar_t inherited[BRW_DEPENDENCIES_CAP];
    const wchar_t *dependencies = install->dependencies;
    if (update) {
        if (!read_dependencies(install, install->active_version, inherited))
            return "dependency-state";
        dependencies = inherited;
    }
    if (dependencies && *dependencies &&
        (!join(record, install->stage, L".brw-depends") ||
         !write_record(install->stage, record, dependencies, false)))
        return "version-record";
    struct launcher_image image;
    if (!select_launcher(install->stage, install->package->entry, &image) ||
        !join(record, install->stage, L".brw-shim") ||
        !write_record(install->stage, record, image.digest, false))
        return "version-record";
    if (update) {
        wchar_t directory[PATH_CAP], entry[ENTRY_CAP];
        if (!version_entry(install, install->active_version, entry) ||
            wcscmp(entry, install->active_entry) ||
            !join(directory, install->package_dir, install->active_version) ||
            !join(record, directory, L".brw-entry"))
            return "entry-conflict";
        if (GetFileAttributesW(record) == INVALID_FILE_ATTRIBUTES &&
            !write_record(directory, record, entry, false))
            return "version-record";
        if (!join(record, directory, L".brw-shim") ||
            (GetFileAttributesW(record) == INVALID_FILE_ATTRIBUTES &&
             !write_record(directory, record, install->shim_hash, false)))
            return "version-record";
    }
    /* Cancellation and publication compete for the same transition. */
    if (install->source &&
        InterlockedCompareExchange(&publish_state, PUBLISHING, RUNNING) == CANCELLED)
        return "cancelled";
    wchar_t receipt[PATH_CAP];
    if (!install_record_path(install, install->package->version, receipt) ||
        !write_record(install->state, receipt, install->version, false))
        return "install-record";
    if (!MoveFileW(install->stage, install->version))
        return DeleteFileW(receipt) ? "publish-version" : "install-record-cleanup";
    install->stage[0] = 0;
    const char *error = switch_entry(install, target, &image, update);
    if (!error || !strcmp(error, "entry-cleanup"))
        return error;
    if (!owned_version(install->version, install->package->entry, install->version) ||
        !remove_stage(install->version))
        return "version-cleanup";
    return DeleteFileW(receipt) ? "publish-entry" : "install-record-cleanup";
}

static const char *activate(struct installation *install)
{
    const char *error = prepare(install);
    if (error)
        return error;
    wchar_t entry[ENTRY_CAP], target[PATH_CAP];
    if (!version_entry(install, install->package->version, entry) ||
        !join(target, install->version, entry))
        return "version-not-installed";
    wchar_t names[BRW_DEPENDENCIES_CAP], dependency[BRW_PACKAGE_NAME_CAP];
    if (!read_dependencies(install, install->package->version, names))
        return "dependency-state";
    const wchar_t *cursor = names;
    while (brw_next_dependency(&cursor, dependency)) {
        if (!brw_is_installed(dependency))
            return "dependency-not-installed";
    }
    struct launcher_image image;
    wchar_t record[PATH_CAP], recorded[65];
    if (!select_launcher(install->version, entry, &image) ||
        !join(record, install->version, L".brw-shim"))
        return "version-record";
    /* A retained version may have been published by an older launcher build. */
    if (!read_record(record, recorded, 65) || wcscmp(recorded, image.digest)) {
        if (!wcscmp(install->active_version, install->package->version))
            return NULL;
        if (!write_record(install->version, record, image.digest, true))
            return "version-record";
    }
    return switch_entry(install, target, &image, true);
}

static bool listed_path(FILE *files, const wchar_t *name, bool folder)
{
    char line[ENTRY_CAP + 2];
    rewind(files);
    while (fgets(line, sizeof line, files)) {
        char *end = strchr(line, '\n');
        wchar_t listed[ENTRY_CAP];
        if (!end || end == line)
            return false;
        *end = 0;
        bool listed_folder = end[-1] == '/';
        if (!archive_path(line, listed_folder, true, listed) || !*listed)
            return false;
        if (folder == listed_folder && !_wcsicmp(name, listed))
            return true;
    }
    return false;
}

static bool removable(const wchar_t *path, bool folder)
{
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (folder ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE file = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, flags, NULL);
    return file != INVALID_HANDLE_VALUE && CloseHandle(file);
}

static bool owned_tree(const wchar_t *directory, const wchar_t *relative, const wchar_t *entry,
                       const wchar_t *detached, FILE *files, unsigned depth)
{
    wchar_t pattern[PATH_CAP];
    if (depth > ARCHIVE_DEPTH || !join(pattern, directory, L"*"))
        return false;
    WIN32_FIND_DATAW file;
    HANDLE search = FindFirstFileW(pattern, &file);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool owned = true;
    do {
        if (dots(file.cFileName))
            continue;
        bool folder = (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool root = !*relative;
        bool inventory = root && !wcscmp(file.cFileName, L".brw-files");
        bool metadata = !folder && (inventory || (root && (!wcscmp(file.cFileName, L".brw-depends") ||
                                                         !wcscmp(file.cFileName, L".brw-entry") ||
                                                         !wcscmp(file.cFileName, L".brw-shim"))));
        wchar_t name[ENTRY_CAP], path[PATH_CAP];
        if (wcslen(relative) + wcslen(file.cFileName) + 2 > ENTRY_CAP) {
            owned = false;
            break;
        }
        swprintf(name, ENTRY_CAP, L"%ls%ls%ls", relative, root ? L"" : L"/", file.cFileName);
        if (wcslen(detached) + wcslen(name) + 1 >= MAX_PATH ||
            (file.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)) ||
            (!metadata && (files ? !listed_path(files, name, folder)
                                 : folder || wcscmp(name, entry) != 0)) ||
            !join(path, directory, file.cFileName) ||
            (!inventory && !removable(path, folder)) ||
            (folder && !owned_tree(path, name, entry, detached, files, depth + 1))) {
            owned = false;
            break;
        }
    } while (FindNextFileW(search, &file));
    owned = owned && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return owned;
}

static bool owned_version(const wchar_t *directory, const wchar_t *entry,
                          const wchar_t *detached)
{
    wchar_t inventory[PATH_CAP];
    if (!join(inventory, directory, L".brw-files"))
        return false;
    DWORD attributes = GetFileAttributesW(inventory);
    FILE *files = NULL;
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (!ordinary_path(inventory, false) || !(files = _wfsopen(inventory, L"rb", _SH_DENYNO))))
        return false;
    bool owned = owned_tree(directory, L"", entry, detached, files, 0);
    if (files)
        fclose(files);
    return owned && (attributes == INVALID_FILE_ATTRIBUTES || removable(inventory, false));
}

/* Check every version before changing the entry. Cleanup only touches detached payloads. */
static bool package_versions(struct installation *install, const wchar_t *directory,
                             const wchar_t *detached)
{
    wchar_t pattern[PATH_CAP];
    if (!join(pattern, directory, L"*"))
        return false;
    WIN32_FIND_DATAW version;
    HANDLE search = FindFirstFileW(pattern, &version);
    if (search == INVALID_HANDLE_VALUE)
        return false;
    bool valid = true;
    do {
        if (dots(version.cFileName))
            continue;
        wchar_t path[PATH_CAP], entry[ENTRY_CAP], destination[PATH_CAP];
        if (!brw_package_component(version.cFileName) || !join(path, directory, version.cFileName) ||
            !ordinary_path(path, true) ||
            (detached ? (!join(destination, detached, version.cFileName) ||
                         !version_entry(install, version.cFileName, entry) ||
                         !owned_version(path, entry, destination)) :
                        (!remove_install_record(install, version.cFileName) || !remove_stage(path)))) {
            valid = false;
            break;
        }
    } while (FindNextFileW(search, &version));
    valid = valid && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return valid;
}

static const char *uninstall(struct installation *install)
{
    const char *error = prepare(install);
    if (error)
        return error;
    wchar_t bin_stage[PATH_CAP] = L"", data_stage[PATH_CAP] = L"";
    wchar_t shim[PATH_CAP], sidecar[PATH_CAP], versions[PATH_CAP];
    error = "uninstall-stage";
    if (!stage_directory(install->bin, bin_stage) ||
        !stage_directory(install->packages, data_stage) ||
        !join(shim, bin_stage, L"entry.exe") || !join(sidecar, bin_stage, L"entry.target") ||
        !join(versions, data_stage, L"versions"))
        goto done;
    error = "package-conflict";
    if (!package_versions(install, install->package_dir, versions))
        goto done;
    error = "uninstall-entry";
    if (!MoveFileW(install->shim, shim))
        goto done;
    if (!MoveFileW(install->sidecar, sidecar)) {
        if (!MoveFileW(shim, install->shim))
            error = "uninstall-restore";
        goto done;
    }
    if (!MoveFileW(install->package_dir, versions)) {
        error = "uninstall-version";
        if (!MoveFileW(sidecar, install->sidecar) || !MoveFileW(shim, install->shim))
            error = "uninstall-restore";
        goto done;
    }
    error = NULL;
    bool clean_shim = DeleteFileW(shim) != 0;
    bool clean_sidecar = DeleteFileW(sidecar) != 0;
    bool clean_versions = package_versions(install, versions, NULL);
    if (!clean_shim || !clean_sidecar || !clean_versions || !RemoveDirectoryW(versions))
        error = "uninstall-detached-cleanup";

done:
    if (bin_stage[0])
        RemoveDirectoryW(bin_stage);
    if (data_stage[0])
        RemoveDirectoryW(data_stage);
    return error;
}

static const char *run_step(void *context, enum digby_operation operation)
{
    struct installation *install = context;
    if (brw_cancelled())
        return "cancelled";
    switch (operation) {
    case DIGBY_DOWNLOAD: return download(install);
    case DIGBY_VERIFY: return verify(install);
    case DIGBY_EXTRACT: return extract(install);
    case DIGBY_PUBLISH: return publish(install);
    case DIGBY_ACTIVATE: return activate(install);
    case DIGBY_REMOVE: return uninstall(install);
    }
    return "unsupported-operation";
}

const char *brw_install_package(const struct local_package *package,
                                const struct digby_plan *plan, size_t *failed_step)
{
    return brw_run_lifecycle(DIGBY_INSTALL, package, plan, failed_step);
}

static const char *run_lifecycle(enum digby_action action, const struct local_package *package,
                                 const struct digby_plan *plan, size_t *failed_step,
                                 bool source, bool executable, const wchar_t *dependencies)
{
    struct installation install = {0};
    install.package = package;
    install.dependencies = dependencies;
    install.action = action;
    install.source = source;
    install.executable = executable;
    if (failed_step)
        *failed_step = 0;
    const char *error = digby_execute(plan, run_step, &install, failed_step);
    if (install.archive)
        fclose(install.archive);
    if (install.download_stage[0])
        remove_stage(install.download_stage);
    if (install.stage[0])
        remove_stage(install.stage);
    if (install.lock && install.lock != INVALID_HANDLE_VALUE)
        CloseHandle(install.lock);
    if (source)
        InterlockedAnd(&publish_state, ~PUBLISHING);
    return error;
}

const char *brw_run_lifecycle(enum digby_action action, const struct local_package *package,
                             const struct digby_plan *plan, size_t *failed_step)
{
    return run_lifecycle(action, package, plan, failed_step, false, false, NULL);
}

const char *brw_install_with_dependencies(const struct local_package *package, bool executable,
                                          const wchar_t *dependencies,
                                          const struct digby_plan *plan, size_t *failed_step)
{
    return run_lifecycle(DIGBY_INSTALL, package, plan, failed_step, true, executable, dependencies);
}

int brw_list_installed(bool json)
{
    struct local_package package = {0};
    struct installation install = {0};
    install.package = &package;
    wchar_t pattern[PATH_CAP];
    const char *error = NULL;
    error = package_roots(&install, false);
    if (!error && !join(pattern, install.packages, L"*"))
        error = "install-directory";

    if (json)
        fputs("{\"packages\":[", stdout);
    bool comma = false;
    if (!error) {
        WIN32_FIND_DATAW data;
        HANDLE search = FindFirstFileW(pattern, &data);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    !brw_package_component(data.cFileName))
                    continue;
                package.name = data.cFileName;
                if (!package_paths(&install) || read_active(&install))
                    continue;
                if (json) {
                    printf("%s{\"name\":", comma ? "," : "");
                    brw_json_string(package.name);
                    fputs(",\"version\":", stdout);
                    brw_json_string(install.active_version);
                    putchar('}');
                    comma = true;
                } else {
                    printf("%ls %ls\
", package.name, install.active_version);
                }
            } while (FindNextFileW(search, &data));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                error = "install-directory";
            FindClose(search);
        } else if (GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND) {
            error = "install-directory";
        }
    }
    if (json) {
        if (error)
            printf("],\"status\":\"failed\",\"error\":\"%s\",\"step\":0}\
", error);
        else
            puts("],\"status\":\"ok\"}");
    } else if (error) {
        fprintf(stderr, "brw: %s\
", error);
    }
    return error ? 1 : 0;
}
