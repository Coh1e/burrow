#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <archive.h>
#include <archive_entry.h>
#include <stdbool.h>
#include <stdio.h>
#include <share.h>
#include <string.h>
#include "install_windows.h"
#include "shim_image.h"
#include "shim_windows.h"

enum { PATH_CAP = BRW_PATH_CAP, NAME_CAP = 128 };

struct installation {
    const struct local_package *package;
    enum digby_action action;
    FILE *archive;
    HANDLE lock;
    wchar_t packages[PATH_CAP];
    wchar_t package_dir[PATH_CAP];
    wchar_t version[PATH_CAP];
    wchar_t bin[PATH_CAP];
    wchar_t shim[PATH_CAP];
    wchar_t sidecar[PATH_CAP];
    wchar_t stage[PATH_CAP];
    wchar_t active_version[NAME_CAP];
    wchar_t active_entry[NAME_CAP];
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

/* This slice accepts flat ASCII package filenames and Windows executables. */
static bool component(const wchar_t *name)
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

const char *brw_validate_package(const struct local_package *package)
{
    if (!component(package->name) || !component(package->version) || !component(package->entry))
        return "invalid-package";
    size_t length = wcslen(package->entry);
    if (length <= 4 || _wcsicmp(package->entry + length - 4, L".exe"))
        return "invalid-entry";
    if (!*package->archive || wcslen(package->digest) != 64)
        return "invalid-input";
    for (size_t i = 0; i < 64; ++i) {
        if (!wcschr(L"0123456789abcdefABCDEF", package->digest[i]))
            return "invalid-digest";
    }
    return NULL;
}

const char *brw_validate_lifecycle(enum digby_action action, const struct local_package *package)
{
    if (action == DIGBY_INSTALL || action == DIGBY_UPDATE)
        return brw_validate_package(package);
    if (!component(package->name) || (action == DIGBY_ROLLBACK && !component(package->version)))
        return "invalid-package";
    return NULL;
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
            if (BCryptHashData(hash, bytes, (ULONG)count, 0) < 0)
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
    install->archive = _wfsopen(install->package->archive, L"rb", _SH_DENYWR);
    if (!install->archive)
        return "archive-open";
    wchar_t digest[65];
    if (!hash_contents(install->archive, NULL, 0, digest) ||
        fseek(install->archive, 0, SEEK_SET) != 0)
        return "digest-read";
    return _wcsicmp(install->package->digest, digest) ? "digest-mismatch" : NULL;
}

static bool directory(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL))
        return true;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool roots(struct installation *install)
{
    wchar_t profile[PATH_CAP];
    DWORD count = GetEnvironmentVariableW(L"USERPROFILE", profile, PATH_CAP);
    if (count < 3 || count >= PATH_CAP || profile[1] != L':' ||
        (profile[2] != L'\\' && profile[2] != L'/'))
        return false;
    wchar_t local[PATH_CAP], share[PATH_CAP], data[PATH_CAP];
    wchar_t alias[NAME_CAP + 12];
    swprintf(alias, NAME_CAP + 12, L"%ls.exe", install->package->name);
    if (!join(local, profile, L".local") || !join(share, local, L"share") ||
        !join(data, share, L"burrow") || !join(install->packages, data, L"packages") ||
        !join(install->package_dir, install->packages, install->package->name) ||
        (install->package->version &&
         !join(install->version, install->package_dir, install->package->version)) ||
        !join(install->bin, local, L"bin") || !join(install->shim, install->bin, alias))
        return false;
    wcscpy(install->sidecar, install->shim);
    if (!brw_append_shim_suffix(install->sidecar, wcslen(install->shim)))
        return false;
    return install->action != DIGBY_INSTALL ||
           (directory(local) && directory(share) && directory(data) &&
            directory(install->packages));
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

static bool ordinary_path(const wchar_t *path, bool expect_directory)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !!(attributes & FILE_ATTRIBUTE_DIRECTORY) == expect_directory;
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
        /* Legacy installs have no shim digest. Compare against the shipped launcher. */
        if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES ||
            !hash_contents(NULL, shim_image, sizeof shim_image, expected))
            return false;
    }
    return !wcscmp(install->shim_hash, expected);
}

static const char *prepare(struct installation *install)
{
    if (!roots(install))
        return "install-directory";
    if (!ordinary_path(install->packages, true))
        return install->action == DIGBY_INSTALL ? "install-directory" : "not-installed";
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

    wchar_t target[PATH_CAP];
    if (!ordinary_path(install->bin, true) || !read_record(install->sidecar, target, PATH_CAP))
        return "entry-conflict";
    size_t prefix = wcslen(install->package_dir);
    if (_wcsnicmp(target, install->package_dir, prefix) || target[prefix] != L'\\')
        return "entry-conflict";
    const wchar_t *relative = target + prefix + 1;
    const wchar_t *separator = wcschr(relative, L'\\');
    if (!separator || (size_t)(separator - relative) >= NAME_CAP || !component(separator + 1))
        return "entry-conflict";
    size_t length = (size_t)(separator - relative);
    wmemcpy(install->active_version, relative, length);
    install->active_version[length] = 0;
    wchar_t version[PATH_CAP];
    if (!component(install->active_version) ||
        !join(version, install->package_dir, install->active_version) ||
        !ordinary_path(version, true) || !ordinary_path(target, false) || !owned_shim(install, version))
        return "entry-conflict";
    wcscpy(install->active_entry, separator + 1);
    wchar_t record[PATH_CAP], recorded_entry[NAME_CAP];
    if (!join(record, version, L".brw-entry"))
        return "entry-conflict";
    if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES &&
        (!read_record(record, recorded_entry, NAME_CAP) ||
         wcscmp(install->active_entry, recorded_entry)))
        return "entry-conflict";
    return NULL;
}

static bool archive_name(const char *input, wchar_t name[NAME_CAP])
{
    if (!input)
        return false;
    size_t i = 0;
    for (; input[i]; ++i) {
        if (i == NAME_CAP - 1 || (unsigned char)input[i] > 127)
            return false;
        name[i] = (unsigned char)input[i];
    }
    name[i] = 0;
    return component(name);
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
        if (!WriteFile(file, buffer, (DWORD)count, &size, NULL) || size != (DWORD)count) {
            written = false;
            break;
        }
    }
    if (!CloseHandle(file))
        written = false;
    return written && count == 0 ? NULL : "archive-data";
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
    struct archive *reader = archive_read_new();
    if (!reader) {
        fclose(files);
        return "archive-open";
    }
    const char *error = "archive-open";
    bool found_entry = false;
    if (archive_read_support_format_tar(reader) == ARCHIVE_OK &&
        archive_read_open_FILE(reader, install->archive) == ARCHIVE_OK) {
        struct archive_entry *entry;
        int status;
        error = NULL;
        while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
            wchar_t name[NAME_CAP], path[PATH_CAP];
            if (!archive_name(archive_entry_pathname(entry), name) ||
                archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
                archive_entry_symlink(entry) || !join(path, install->stage, name)) {
                error = "archive-path";
                break;
            }
            error = extract_file(reader, path);
            if (error)
                break;
            if (fprintf(files, "%s\n", archive_entry_pathname(entry)) < 0) {
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

static bool remove_stage(const wchar_t *stage)
{
    wchar_t path[PATH_CAP];
    if (!join(path, stage, L"*"))
        return false;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(path, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                join(path, stage, entry.cFileName))
                DeleteFileW(path);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    return RemoveDirectoryW(stage) != 0;
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

static bool version_entry(const struct installation *install, const wchar_t *version,
                          wchar_t entry[NAME_CAP])
{
    wchar_t directory[PATH_CAP], record[PATH_CAP], target[PATH_CAP];
    if (!join(directory, install->package_dir, version) || !ordinary_path(directory, true) ||
        !join(record, directory, L".brw-entry"))
        return false;
    if (!read_record(record, entry, NAME_CAP)) {
        if (GetFileAttributesW(record) != INVALID_FILE_ATTRIBUTES ||
            wcscmp(version, install->active_version))
            return false;
        /* The original installer recorded its entry only in the active sidecar. */
        wcscpy(entry, install->active_entry);
    }
    return component(entry) && join(target, directory, entry) && ordinary_path(target, false);
}

static const char *publish(struct installation *install)
{
    wchar_t target[PATH_CAP], record[PATH_CAP];
    if (!join(target, install->version, install->package->entry) || !directory(install->bin))
        return "entry-directory";
    if (!join(record, install->stage, L".brw-entry") ||
        !write_record(install->stage, record, install->package->entry, false))
        return "version-record";
    bool update = install->action == DIGBY_UPDATE;
    if ((!update && !hash_contents(NULL, shim_image, sizeof shim_image, install->shim_hash)) ||
        !join(record, install->stage, L".brw-shim") ||
        !write_record(install->stage, record, install->shim_hash, false))
        return "version-record";
    if (update) {
        wchar_t directory[PATH_CAP], entry[NAME_CAP];
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
    if (!MoveFileW(install->stage, install->version))
        return "publish-version";
    install->stage[0] = 0;
    if (write_record(install->bin, install->sidecar, target, update)) {
        if (update || publish_file(install->bin, install->shim, shim_image, sizeof shim_image, false))
            return NULL;
        if (!DeleteFileW(install->sidecar))
            return "entry-cleanup";
    }
    return remove_stage(install->version) ? "publish-entry" : "version-cleanup";
}

static const char *activate(struct installation *install)
{
    const char *error = prepare(install);
    if (error)
        return error;
    wchar_t entry[NAME_CAP], target[PATH_CAP];
    if (!version_entry(install, install->package->version, entry) ||
        !join(target, install->version, entry))
        return "version-not-installed";
    return write_record(install->bin, install->sidecar, target, true) ? NULL : "publish-entry";
}

static bool dots(const wchar_t *name)
{
    return !wcscmp(name, L".") || !wcscmp(name, L"..");
}

static bool listed_file(FILE *files, const wchar_t *name)
{
    char line[NAME_CAP + 1];
    rewind(files);
    while (fgets(line, sizeof line, files)) {
        char *end = strchr(line, '\n');
        wchar_t listed[NAME_CAP];
        if (!end)
            return false;
        *end = '\0';
        if (!archive_name(line, listed))
            return false;
        if (!wcscmp(name, listed))
            return true;
    }
    return false;
}

static bool removable_file(const wchar_t *directory, const wchar_t *name)
{
    wchar_t path[PATH_CAP];
    if (!join(path, directory, name))
        return false;
    HANDLE file = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    return CloseHandle(file) != 0;
}

static bool flat_version(const wchar_t *directory, const wchar_t *entry,
                         const wchar_t *detached)
{
    wchar_t pattern[PATH_CAP], inventory[PATH_CAP];
    if (!join(pattern, directory, L"*") || !join(inventory, directory, L".brw-files"))
        return false;
    DWORD attributes = GetFileAttributesW(inventory);
    FILE *files = NULL;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!ordinary_path(inventory, false) || !(files = _wfsopen(inventory, L"rb", _SH_DENYNO)))
            return false;
    }
    WIN32_FIND_DATAW file;
    HANDLE search = FindFirstFileW(pattern, &file);
    if (search == INVALID_HANDLE_VALUE) {
        if (files)
            fclose(files);
        return false;
    }
    bool flat = true;
    do {
        if (dots(file.cFileName))
            continue;
        bool metadata = !wcscmp(file.cFileName, L".brw-entry") ||
                        !wcscmp(file.cFileName, L".brw-shim") ||
                        !wcscmp(file.cFileName, L".brw-files");
        if (wcslen(detached) + wcslen(file.cFileName) + 1 >= MAX_PATH ||
            (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                     FILE_ATTRIBUTE_READONLY)) ||
            (!metadata && (files ? !listed_file(files, file.cFileName)
                                 : wcscmp(file.cFileName, entry) != 0)) ||
            (wcscmp(file.cFileName, L".brw-files") &&
             !removable_file(directory, file.cFileName))) {
            flat = false;
            break;
        }
    } while (FindNextFileW(search, &file));
    flat = flat && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    if (files)
        fclose(files);
    return flat && (attributes == INVALID_FILE_ATTRIBUTES ||
                    removable_file(directory, L".brw-files"));
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
        wchar_t path[PATH_CAP], entry[NAME_CAP], destination[PATH_CAP];
        if (!component(version.cFileName) || !join(path, directory, version.cFileName) ||
            !ordinary_path(path, true) ||
            (detached ? (!join(destination, detached, version.cFileName) ||
                         !version_entry(install, version.cFileName, entry) ||
                         !flat_version(path, entry, destination)) : !remove_stage(path))) {
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
    switch (operation) {
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

const char *brw_run_lifecycle(enum digby_action action, const struct local_package *package,
                             const struct digby_plan *plan, size_t *failed_step)
{
    struct installation install = {0};
    install.package = package;
    install.action = action;
    const char *error = digby_execute(plan, run_step, &install, failed_step);
    if (install.archive)
        fclose(install.archive);
    if (install.stage[0])
        remove_stage(install.stage);
    if (install.lock && install.lock != INVALID_HANDLE_VALUE)
        CloseHandle(install.lock);
    return error;
}
