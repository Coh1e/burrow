#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <archive.h>
#include <archive_entry.h>
#include <stdbool.h>
#include <stdio.h>
#include <share.h>
#include <wctype.h>
#include "install_windows.h"
#include "shim_image.h"
#include "shim_windows.h"

enum { PATH_CAP = BRW_PATH_CAP, NAME_CAP = 128 };

struct installation {
    const struct local_package *package;
    FILE *archive;
    wchar_t package_dir[PATH_CAP];
    wchar_t version[PATH_CAP];
    wchar_t bin[PATH_CAP];
    wchar_t shim[PATH_CAP];
    wchar_t sidecar[PATH_CAP];
    wchar_t stage[PATH_CAP];
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

static const char *verify(struct installation *install)
{
    install->archive = _wfsopen(install->package->archive, L"rb", _SH_DENYWR);
    if (!install->archive)
        return "archive-open";
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    const char *error = "digest-read";
    unsigned char bytes[8192];
    unsigned char digest[32];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0)
        goto done;
    size_t count;
    while ((count = fread(bytes, 1, sizeof bytes, install->archive)) != 0) {
        if (BCryptHashData(hash, bytes, (ULONG)count, 0) < 0)
            goto done;
    }
    if (ferror(install->archive) || BCryptFinishHash(hash, digest, sizeof digest, 0) < 0 ||
        fseek(install->archive, 0, SEEK_SET) != 0)
        goto done;
    error = NULL;
    for (size_t i = 0; i < sizeof digest; ++i) {
        const wchar_t *hex = L"0123456789abcdef";
        if (towlower(install->package->digest[i * 2]) != hex[digest[i] >> 4] ||
            towlower(install->package->digest[i * 2 + 1]) != hex[digest[i] & 15]) {
            error = "digest-mismatch";
            break;
        }
    }

done:
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    return error;
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
    wchar_t local[PATH_CAP], share[PATH_CAP], data[PATH_CAP], packages[PATH_CAP];
    wchar_t alias[NAME_CAP + 12];
    swprintf(alias, NAME_CAP + 12, L"%ls.exe", install->package->name);
    if (!join(local, profile, L".local") || !join(share, local, L"share") ||
        !join(data, share, L"burrow") || !join(packages, data, L"packages") ||
        !join(install->package_dir, packages, install->package->name) ||
        !join(install->version, install->package_dir, install->package->version) ||
        !join(install->bin, local, L"bin") || !join(install->shim, install->bin, alias))
        return false;
    wcscpy(install->sidecar, install->shim);
    if (!brw_append_shim_suffix(install->sidecar, wcslen(install->shim)))
        return false;
    return directory(local) && directory(share) && directory(data) &&
           directory(packages) && directory(install->package_dir);
}

static bool stage_directory(struct installation *install)
{
    wchar_t name[64];
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        swprintf(name, 64, L".stage-%lu-%llu-%u", GetCurrentProcessId(),
                 GetTickCount64(), attempt);
        if (!join(install->stage, install->package_dir, name))
            break;
        if (CreateDirectoryW(install->stage, NULL))
            return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    install->stage[0] = 0;
    return false;
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
    if (!roots(install))
        return "install-directory";
    if (GetFileAttributesW(install->version) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(install->shim) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(install->sidecar) != INVALID_FILE_ATTRIBUTES)
        return "destination-exists";
    if (!stage_directory(install))
        return "stage-directory";
    struct archive *reader = archive_read_new();
    if (!reader)
        return "archive-open";
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
            if (!wcscmp(name, install->package->entry))
                found_entry = true;
        }
        if (!error && status != ARCHIVE_EOF)
            error = "archive-data";
        if (!error && !found_entry)
            error = "entry-missing";
    }
    archive_read_free(reader);
    return error;
}

static void remove_stage(const wchar_t *stage)
{
    wchar_t path[PATH_CAP];
    if (!join(path, stage, L"*"))
        return;
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
    RemoveDirectoryW(stage);
}

/* Create a complete file under a staging name before making it visible. */
static bool publish_file(const wchar_t *bin, const wchar_t *path, const void *data, DWORD length)
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
    if (success && MoveFileW(temporary, path))
        return true;
    DeleteFileW(temporary);
    return false;
}

static const char *publish(struct installation *install)
{
    wchar_t target[PATH_CAP];
    if (!join(target, install->version, install->package->entry) || !directory(install->bin))
        return "entry-directory";
    if (!MoveFileW(install->stage, install->version))
        return "publish-version";
    install->stage[0] = 0;
    DWORD size = (DWORD)((wcslen(target) + 1) * sizeof *target);
    if (publish_file(install->bin, install->sidecar, target, size)) {
        if (publish_file(install->bin, install->shim, shim_image, sizeof shim_image))
            return NULL;
        DeleteFileW(install->sidecar);
    }
    remove_stage(install->version);
    return "publish-entry";
}

static const char *run_step(void *context, enum digby_operation operation)
{
    struct installation *install = context;
    switch (operation) {
    case DIGBY_VERIFY: return verify(install);
    case DIGBY_EXTRACT: return extract(install);
    case DIGBY_PUBLISH: return publish(install);
    }
    return "unsupported-operation";
}

const char *brw_install_package(const struct local_package *package,
                                const struct digby_plan *plan, size_t *failed_step)
{
    struct installation install = {0};
    install.package = package;
    const char *error = digby_execute(plan, run_step, &install, failed_step);
    if (install.archive)
        fclose(install.archive);
    if (install.stage[0])
        remove_stage(install.stage);
    return error;
}
