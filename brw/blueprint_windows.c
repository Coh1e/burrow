#include "blueprint_edit.h"
#include "install_windows.h"
#include "settings_backend_windows.h"
#include <winternl.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "blueprint_files_windows.h"
#include "command_line_windows.h"
#include <bcrypt.h>

bool bp_create_output_file(struct bp_output_file *file)
{
    wchar_t path[32768];
    DWORD n = GetTempPathW(32768, path);
    ULONGLONG id;
    if (!n || n >= 32768 || BCryptGenRandom(NULL, (PUCHAR)&id, sizeof id,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) ||
        swprintf(path + n, 32768 - n, L"brw-output-%016llx", id) < 0) return false;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    file->path = bytes ? malloc((size_t)bytes) : NULL;
    if (!file->path) return false;
    WideCharToMultiByte(CP_UTF8, 0, path, -1, file->path, bytes, NULL, NULL);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
        if (fd < 0) CloseHandle(h);
        else {
            file->stream = _fdopen(fd, "rb");
            if (file->stream) return true;
            _close(fd);
        }
        DeleteFileW(path);
    }
    free(file->path);
    file->path = NULL;
    return false;
}

bool bp_remove_output_file(struct bp_output_file *file)
{
    wchar_t *path = wide(file->path);
    bool ok = fclose(file->stream) == 0;
    if (!path || !DeleteFileW(path)) ok = false;
    free(path);
    free(file->path);
    return ok;
}

static HANDLE open_parent(const char *workspace, wchar_t *path, wchar_t **leaf)
{
    wchar_t *root = wide(workspace);
    HANDLE dir = root ? open_directory(root) : INVALID_HANDLE_VALUE;
    free(root);
    *leaf = path;
    wchar_t *slash;
    while (dir != INVALID_HANDLE_VALUE && (slash = wcschr(*leaf, L'/')) != NULL) {
        *slash = 0;
        HANDLE next = open_child(dir, *leaf, FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_DIRECTORY_FILE);
        CloseHandle(dir);
        dir = next;
        if (dir != INVALID_HANDLE_VALUE && !ordinary(dir, true)) {
            CloseHandle(dir);
            dir = INVALID_HANDLE_VALUE;
        }
        *leaf = slash + 1;
    }
    return dir;
}

const char *bp_compare_output_case(const char *workspace, const char *a,
                                   const char *b, enum bp_output_match *match)
{
    wchar_t *pa = wide(a), *pb = wide(b), *la = NULL, *lb = NULL;
    HANDLE da = pa ? open_parent(workspace, pa, &la) : INVALID_HANDLE_VALUE;
    HANDLE db = pb ? open_parent(workspace, pb, &lb) : INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION ia, ib;
    const char *error = "inspect-output";
    if (da != INVALID_HANDLE_VALUE && db != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(da, &ia) && GetFileInformationByHandle(db, &ib)) {
        error = NULL;
        bool same_dir = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
                        ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
        *match = same_dir ? BP_SAME_OUTPUT : BP_DISTINCT_OUTPUTS;
        if (same_dir && wcscmp(la, lb)) {
            FILE_CASE_SENSITIVE_INFO info;
            /* FileCaseSensitiveInfo is omitted by SDKs targeting older Windows. */
            if (GetFileInformationByHandleEx(da, (FILE_INFO_BY_HANDLE_CLASS)23, &info, sizeof info))
                *match = (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) ? BP_DISTINCT_OUTPUTS : BP_SAME_OUTPUT;
            else {
                DWORD flags;
                if (!GetVolumeInformationByHandleW(da, NULL, 0, NULL, NULL, &flags, NULL, 0) ||
                    (flags & FILE_CASE_SENSITIVE_SEARCH)) *match = BP_UNKNOWN_CASE;
            }
        }
    }
    if (da != INVALID_HANDLE_VALUE) CloseHandle(da);
    if (db != INVALID_HANDLE_VALUE) CloseHandle(db);
    free(pa);
    free(pb);
    return error;
}

static HANDLE new_stage(HANDLE parent, bool temporary, const wchar_t *target)
{
    for (unsigned i = 0; i < 64; ++i) {
        wchar_t name[64];
        swprintf(name, 64, L".brw-bp-%lu-%llu-%u", GetCurrentProcessId(), GetTickCount64(), i);
        if (target && !_wcsicmp(name, target)) continue;
        HANDLE file = open_child(parent, name, GENERIC_READ | GENERIC_WRITE | DELETE, 0,
            FILE_CREATE, FILE_NON_DIRECTORY_FILE | (temporary ? FILE_DELETE_ON_CLOSE : 0));
        if (file != INVALID_HANDLE_VALUE || (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS)) return file;
    }
    return INVALID_HANDLE_VALUE;
}

/* The returned stream owns h, including when fdopen fails. */
static FILE *stream(HANDLE h)
{
    int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE *f = _fdopen(fd, "r+b");
    if (!f) _close(fd);
    return f;
}

static const char *read_text(HANDLE original, char **out, size_t *length)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(original, &size) || size.QuadPart < 0 || size.QuadPart > BP_TEXT_LIMIT)
        return "text-limit";
    DWORD n = size.LowPart, got;
    char *text = malloc((size_t)n + 1);
    if (!text) return "out-of-memory";
    text[n] = 0;
    if (!ReadFile(original, text, n, &got, NULL) || got != n) {
        free(text);
        return "read-text";
    }
    *out = text;
    *length = n;
    return NULL;
}

const char *bp_read_workspace_text(const char *workspace, const char *relative, char **out, size_t *size)
{
    wchar_t *path = wide(relative), *leaf = NULL;
    HANDLE dir = path ? open_parent(workspace, path, &leaf) : INVALID_HANDLE_VALUE;
    const char *error = "unsafe-parent";
    if (dir != INVALID_HANDLE_VALUE) {
        HANDLE file = open_child(dir, leaf, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        error = "open-text";
        if (file != INVALID_HANDLE_VALUE) {
            error = ordinary(file, false) ? read_text(file, out, size) : "unsafe-output-path";
            CloseHandle(file);
        }
        CloseHandle(dir);
    }
    free(path);
    return error;
}

static const char *publish(HANDLE stage, const wchar_t *name, bool replace_existing)
{
    size_t bytes = wcslen(name) * sizeof *name;
    ULONG size = (ULONG)(sizeof(FILE_RENAME_INFORMATION) + bytes);
    FILE_RENAME_INFORMATION *info = calloc(1, size);
    if (!info) return "out-of-memory";
    info->ReplaceIfExists = replace_existing;
    /* A leaf name with no RootDirectory renames within the stage's directory. */
    info->FileNameLength = (ULONG)bytes;
    memcpy(info->FileName, name, bytes);
    IO_STATUS_BLOCK io;
    NTSTATUS rc = NtSetInformationFile(stage, &io, info, size, FileRenameInformation);
    free(info);
    return rc < 0 ? "publish-file" : NULL;
}

static const char *file_op(const char *workspace, const struct bp_step *s, bool apply)
{
    const char *error = bp_validate_file_operands(s);
    if (error) return error;
    wchar_t *path = wide(s->path), *name = NULL;
    HANDLE dir = path ? open_parent(workspace, path, &name) : INVALID_HANDLE_VALUE;
    HANDLE old = INVALID_HANDLE_VALUE, stage = INVALID_HANDLE_VALUE;
    error = "unsafe-parent";
    if (dir == INVALID_HANDLE_VALUE) goto done;
    old = open_child(dir, name, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (old != INVALID_HANDLE_VALUE) {
        error = "unsafe-output-path";
        if (!ordinary(old, false)) goto done;
        error = "destination-exists";
        if (s->op == BP_COPY) goto done;
    } else {
        error = "inspect-output";
        if (GetLastError() != ERROR_FILE_NOT_FOUND) goto done;
        if (s->op == BP_REPLACE && apply) { error = "open-text"; goto done; }
    }
    error = NULL;
    if (!apply) goto done;
    stage = new_stage(dir, false, name);
    if (stage == INVALID_HANDLE_VALUE) { error = "create-stage"; goto done; }
    HANDLE dup;
    error = "open-stage";
    if (!DuplicateHandle(GetCurrentProcess(), stage, GetCurrentProcess(), &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) goto done;
    FILE *out = stream(dup);
    if (!out) goto done;
    if (s->op == BP_REPLACE) {
        char *text = NULL;
        size_t size = 0;
        error = read_text(old, &text, &size);
        if (!error) error = bp_replace_text(out, s, text, size);
        free(text);
    } else error = fwrite(s->payload->data, 1, s->payload->size, out) == s->payload->size ?
                 NULL : "write-stage";
    if (fclose(out) && !error) error = "close-stage";
    if (old != INVALID_HANDLE_VALUE) { CloseHandle(old); old = INVALID_HANDLE_VALUE; }
    if (!error) error = publish(stage, name, s->op == BP_REPLACE);
done:
    if (stage != INVALID_HANDLE_VALUE) {
        if (error) {
            FILE_DISPOSITION_INFO remove = {TRUE};
            if (!SetFileInformationByHandle(stage, FileDispositionInfo, &remove, sizeof remove))
                error = "cleanup-stage";
        }
        CloseHandle(stage);
    }
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    free(path);
    return error;
}

static FILE *payload_stream(const struct bp_file *payload)
{
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    if (!n || n > MAX_PATH) return NULL;
    HANDLE dir = open_directory(temp);
    if (dir == INVALID_HANDLE_VALUE) return NULL;
    HANDLE file = new_stage(dir, true, NULL);
    CloseHandle(dir);
    FILE *f = file == INVALID_HANDLE_VALUE ? NULL : stream(file);
    if (f && (fwrite(payload->data, 1, payload->size, f) != payload->size || fseek(f, 0, SEEK_SET))) {
        fclose(f);
        f = NULL;
    }
    return f;
}

static const char *install(const struct bp_step *s, bool apply)
{
    if (!s->payload || !s->payload->data || s->payload->module) return "invalid-payload";
    wchar_t *name = wide(s->name), *version = wide(s->version), *entry = wide(s->entry);
    wchar_t *path = wide(s->payload->path), *digest = wide(s->payload->digest);
    const struct local_package pkg = {name, version, path, digest, entry};
    const wchar_t *ext = path ? wcsrchr(path, L'.') : NULL;
    bool executable = ext && !_wcsicmp(ext, L".exe");
    const char *error = name && version && entry && path && digest ?
        brw_validate_source_package(&pkg, executable) : "invalid-package";
    if (!error && apply) {
        error = brw_begin_package_changes();
        if (!error) {
            if (!brw_begin_source_install()) error = "cancel-handler";
            else {
                FILE *input = payload_stream(s->payload);
                error = input ? brw_install_stream(&pkg, executable, input) : "payload-stage";
                brw_end_source_install();
            }
            brw_end_package_changes();
        }
    }
    free(digest);
    free(path);
    free(entry);
    free(version);
    free(name);
    return error;
}

static const char *launch_command(const struct bp_step *s, const wchar_t *cwd, wchar_t *script, bool apply)
{
    wchar_t *args[BP_MAX_ARGS + 1] = {wide(s->program)};
    if (args[0] && wcspbrk(args[0], L"/\\") && args[0][0] != L'/' &&
        args[0][0] != L'\\' && args[0][1] != L':') {
        size_t length = wcslen(cwd) + 1 + wcslen(args[0]) + 1;
        wchar_t *joined = malloc(length * sizeof *joined);
        if (joined) swprintf(joined, length, L"%ls\\%ls", cwd, args[0]);
        free(args[0]);
        args[0] = joined;
    }
    const char *error = args[0] ? NULL : "invalid-command";
    for (size_t i = 0; !error && i < s->argc; ++i) {
        args[i + 1] = s->payload && i == s->script_arg ? script : wide(s->args[i]);
        if (!args[i + 1]) error = "invalid-command";
    }
    wchar_t command[32768];
    if (!error) error = encode_command_line(command, s->argc + 1, (const wchar_t *const *)args);
    for (size_t i = 0; i <= s->argc; ++i)
        if (args[i] != script) free(args[i]);
    if (error || !apply) return error;
    STARTUPINFOW start = {0};
    start.cb = sizeof start;
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child;
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, cwd, &start, &child))
        return "child-launch";
    CloseHandle(child.hThread);
    DWORD code = 0;
    bool ok = WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    if (!ok) return "child-wait";
    return code ? "child-failed" : NULL;
}

static const char *run_script(const struct bp_step *s, const wchar_t *cwd, bool apply)
{
    wchar_t path[32768];
    DWORD prefix = GetTempPathW(32768, path);
    ULONGLONG id = 0;
    if (!prefix || prefix >= 32768 ||
        (apply && BCryptGenRandom(NULL, (PUCHAR)&id, sizeof id, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
        swprintf(path + prefix, 32768 - prefix, L"brw-%016llx", id) < 0) return "payload-stage";
    size_t dir_length = wcslen(path);
    const char *base = strrchr(s->payload->path, '/');
    wchar_t *name = wide(base ? base + 1 : s->payload->path);
    if (!name || dir_length + 2 + wcslen(name) > 32768 ||
        (apply && !CreateDirectoryW(path, NULL))) {
        free(name);
        return "payload-stage";
    }
    path[dir_length] = L'\\';
    wcscpy(path + dir_length + 1, name);
    free(name);
    if (!apply) return launch_command(s, cwd, path, false);
    const char *error = "payload-stage";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        bool ok = WriteFile(file, s->payload->data, (DWORD)s->payload->size, &written, NULL) &&
                  written == s->payload->size;
        if (CloseHandle(file) && ok) error = launch_command(s, cwd, path, true);
        if (!DeleteFileW(path) && !error) error = "cleanup-stage";
    }
    path[dir_length] = 0;
    if (!RemoveDirectoryW(path) && !error) error = "cleanup-stage";
    return error;
}

static const char *run_command(const char *workspace, const struct bp_step *s, bool apply)
{
    wchar_t *cwd = wide(workspace);
    DWORD attrs = cwd ? GetFileAttributesW(cwd) : INVALID_FILE_ATTRIBUTES;
    const char *error = NULL;
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        error = "invalid-workspace";
    else
        error = s->payload ? run_script(s, cwd, apply) : launch_command(s, cwd, NULL, apply);
    free(cwd);
    return error;
}

static const char *operation(const char *workspace, const struct bp_step *s, bool apply)
{
    if (!workspace || !*workspace || !s) return "invalid-step";
    switch (s->op) {
    case BP_COPY:
    case BP_REPLACE: return file_op(workspace, s, apply);
    case BP_INSTALL: return install(s, apply);
    case BP_RUN: return run_command(workspace, s, apply);
    case BP_ENV: {
        if (!bp_env_name(s->name) || !s->value) return "invalid-environment";
        wchar_t *name = wide(s->name), *value = wide(s->value);
        struct setting_request r = {0};
        r.preview = !apply;
        struct digby_setting setting = {SETTING_SESSION, DIGBY_ENV_SET, SETTING_STRING,
                                        NULL, name, value, NULL, false};
        struct digby_setting_plan plan;
        const char *error = name && value ?
            digby_plan_setting(&setting, DIGBY_CAP_SESSION_ENV, &plan) : "invalid-environment";
        if (!error) error = digby_execute_setting(&plan, brw_apply_setting, &r);
        free(name);
        free(value);
        return error;
    }
    default: return "missing-capability";
    }
}

const char *bp_validate_step(const char *workspace, const struct bp_step *s)
{
    return operation(workspace, s, false);
}

const char *bp_apply_step(const char *workspace, const struct bp_step *s)
{
    return operation(workspace, s, true);
}
