#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ktmw32.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings_windows.h"
#include "settings_backend_windows.h"
#include "paths_windows.h"
#include "init_windows.h"
#include "install_windows.h"

static DWORD native_type(const struct setting_request *r)
{
    return r->setting.type == SETTING_DWORD ? REG_DWORD : REG_SZ;
}

enum key_access { QUERY_VALUE, CHECK_WRITE, APPLY_WRITE };

/* Open each component itself. Keep its handle while resolving the next one.
   An existing link never grants access to its target. The final transaction
   is enlisted on the verified handle, or creates new components beneath it. */
static LONG open_target(HKEY root, enum digby_setting_scope scope, enum key_access mode,
                        HANDLE transaction, HKEY *result)
{
    const wchar_t *parts[] = {L"Software", L"Burrow", L"Settings"};
    unsigned count = 3;
    if (scope == SETTING_USER) { parts[0] = L"Environment"; count = 1; }
    HKEY parent = root;
    bool new_parent = false;
    LONG status = ERROR_SUCCESS;
    *result = NULL;
    for (unsigned i = 0; i < count; ++i) {
        HKEY next = NULL;
        bool last = i + 1 == count;
        REGSAM access = KEY_QUERY_VALUE | (last && mode != QUERY_VALUE ? KEY_SET_VALUE : 0);
        status = new_parent ? ERROR_FILE_NOT_FOUND :
                 RegOpenKeyExW(parent, parts[i], REG_OPTION_OPEN_LINK, access, &next);
        if (status == ERROR_FILE_NOT_FOUND && mode == CHECK_WRITE) {
            HKEY writable;
            status = RegOpenKeyExW(parent, L"", 0, KEY_CREATE_SUB_KEY, &writable);
            if (status == ERROR_SUCCESS && writable != parent) RegCloseKey(writable);
            break;
        }
        if (status == ERROR_FILE_NOT_FOUND && mode == APPLY_WRITE) {
            DWORD disposition;
            if (!last) access |= KEY_CREATE_SUB_KEY;
            status = transaction ?
                RegCreateKeyTransactedW(parent, parts[i], 0, NULL, 0, access, NULL, &next, &disposition, transaction, NULL) :
                RegCreateKeyExW(parent, parts[i], 0, NULL, 0, access, NULL, &next, &disposition);
            if (status == ERROR_SUCCESS && disposition != REG_CREATED_NEW_KEY) status = ERROR_TRANSACTIONAL_CONFLICT;
            new_parent = true;
        } else if (status == ERROR_SUCCESS) {
            DWORD type, bytes = 0;
            LONG probe = RegQueryValueExW(next, L"SymbolicLinkValue", NULL, &type, NULL, &bytes);
            if (probe != ERROR_FILE_NOT_FOUND && (probe != ERROR_SUCCESS || type == REG_LINK))
                status = ERROR_ACCESS_DENIED;
            if (status == ERROR_SUCCESS && transaction && last) {
                HKEY enlisted;
                status = RegOpenKeyTransactedW(next, L"", 0, access, &enlisted, transaction, NULL);
                RegCloseKey(next);
                next = status == ERROR_SUCCESS ? enlisted : NULL;
            }
        }
        if (parent != root) RegCloseKey(parent);
        parent = next;
        if (status != ERROR_SUCCESS) break;
        if (last) { *result = next; return ERROR_SUCCESS; }
    }
    if (parent && parent != root) RegCloseKey(parent);
    return status;
}

static bool exclusive_hive;

static HKEY registry_root(void)
{
#ifdef BRW_TEST_INTERRUPTS
    static HKEY test_root;
    wchar_t id[33], key[96];
    DWORD length = GetEnvironmentVariableW(L"BRW_TEST_REGISTRY_ID", id, 33);
    if (length) {
        if (length != 32 || wcsspn(id, L"0123456789abcdef") != 32) return NULL;
        swprintf(key, 96, L"Software\\Burrow-Tests-%ls", id);
        if (!test_root && RegOpenKeyExW(HKEY_CURRENT_USER, key, REG_OPTION_OPEN_LINK,
                                        KEY_QUERY_VALUE, &test_root) != ERROR_SUCCESS)
            return NULL;
        return test_root;
    }
    static HKEY private_root;
    wchar_t path[BRW_PATH_CAP];
    DWORD n = GetEnvironmentVariableW(L"BRW_TEST_HIVE", path, BRW_PATH_CAP);
    if (n) {
        if (n >= BRW_PATH_CAP || (!private_root &&
            RegLoadAppKeyW(path, &private_root, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0) != ERROR_SUCCESS))
            return NULL;
        exclusive_hive = true;
        return private_root;
    }
#endif
    return HKEY_CURRENT_USER;
}

static bool environment_name(const wchar_t *name)
{
    return *name && wcslen(name) < SETTING_CAP && !wcschr(name, L'=');
}

static const char *allowed(enum digby_setting_scope scope, const wchar_t *name, DWORD type)
{
    if (scope == SETTING_USER || scope == SETTING_SESSION)
        return environment_name(name) && (type == REG_SZ || type == REG_EXPAND_SZ) ? NULL : "invalid-environment";
    if (scope != SETTING_REGISTRY) return "unauthorized-key";
    if (!_wcsicmp(name, L"Profile")) return type == REG_SZ ? NULL : "value-type";
    if (!_wcsicmp(name, L"Enabled")) return type == REG_DWORD ? NULL : "value-type";
    return "unauthorized-value";
}

static const char *brw_parse_value(DWORD type, const wchar_t *input, struct setting_value *out)
{
    if (!input) return "invalid-value";
    out->type = type;
    if (type == REG_DWORD) {
        DWORD n = 0;
        if (!*input) return "invalid-dword";
        for (; *input; ++input) {
            if (*input < L'0' || *input > L'9' || n > (MAXDWORD - (*input - L'0')) / 10)
                return "invalid-dword";
            n = n * 10 + (*input - L'0');
        }
        out->data.number = n;
    } else {
        if (wcslen(input) >= SETTING_CAP) return "value-too-long";
        wcscpy(out->data.text, input);
    }
    return NULL;
}

static DWORD value_size(const struct setting_value *v)
{
    if (!v->type) return 0;
    if (v->type == REG_DWORD) return sizeof(DWORD);
    return (DWORD)((wcslen(v->data.text) + 1) * sizeof(wchar_t));
}

static bool equal(const struct setting_value *a, const struct setting_value *b)
{
    if (a->type != b->type) return false;
    DWORD size = value_size(a);
    return size == value_size(b) && !memcmp(&a->data, &b->data, size);
}

static const char *read_open_value(HKEY key, enum digby_setting_scope scope, const wchar_t *name, struct setting_value *v)
{
    DWORD type, bytes = sizeof v->data.text;
    LONG status = RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v->data, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return NULL;
    if (status == ERROR_MORE_DATA) return "value-too-long";
    if (status != ERROR_SUCCESS) return "registry-read";
    if (type == REG_SZ || (scope == SETTING_USER && type == REG_EXPAND_SZ)) {
        if (!bytes || bytes % sizeof(wchar_t) || v->data.text[bytes/sizeof(wchar_t)-1] ||
            wcslen(v->data.text) + 1 != bytes / sizeof(wchar_t)) return "value-type";
    } else if (type != REG_DWORD || bytes != sizeof(DWORD)) return "value-type";
    v->type = type;
    return NULL;
}

static const char *read_value(HKEY root, enum digby_setting_scope scope, const wchar_t *name, struct setting_value *v)
{
    v->type = REG_NONE;
    if (scope == SETTING_SESSION) {
        SetLastError(ERROR_SUCCESS);
        DWORD n = GetEnvironmentVariableW(name, v->data.text, SETTING_CAP);
        if (!n && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return NULL;
        if (n >= SETTING_CAP || (!n && GetLastError() != ERROR_SUCCESS)) return "environment-read";
        v->type = REG_SZ;
        return NULL;
    }
    HKEY key;
    LONG status = open_target(root, scope, QUERY_VALUE, NULL, &key);
    if (status == ERROR_FILE_NOT_FOUND) return NULL;
    if (status != ERROR_SUCCESS) return "registry-read";
    const char *error = read_open_value(key, scope, name, v);
    RegCloseKey(key);
    return error;
}

static const char *write_value(HKEY root, enum digby_setting_scope scope, const wchar_t *name,
                               const struct setting_value *expected, const struct setting_value *v)
{
    if (scope == SETTING_SESSION)
        return SetEnvironmentVariableW(name, v->type ? v->data.text : NULL) ? NULL : "environment-write";
    HANDLE transaction = NULL;
    HKEY key;
    /* The private test hive is process-exclusive and has no KTM resource manager. */
    if (!exclusive_hive) {
        transaction = CreateTransaction(NULL, NULL, 0, 0, 0, 0, NULL);
        if (transaction == INVALID_HANDLE_VALUE) return "registry-transaction";
    }
    LONG status = open_target(root, scope, APPLY_WRITE, transaction, &key);
    const char *error = status == ERROR_SUCCESS ? NULL :
                        status == ERROR_TRANSACTIONAL_CONFLICT ? "old-value-conflict" : "registry-write";
    if (!error) {
        struct setting_value actual = {0};
        error = read_open_value(key, scope, name, &actual);
        if (!error && !equal(&actual, expected)) error = "old-value-conflict";
        if (!error) {
            status = v->type ? RegSetValueExW(key, name, 0, v->type, (const BYTE *)&v->data, value_size(v)) :
                               RegDeleteValueW(key, name);
            if (status != ERROR_SUCCESS) error = "registry-write";
        }
        RegCloseKey(key);
    }
    if (transaction) {
        if (!error && !CommitTransaction(transaction)) error = "registry-commit";
        if (error) RollbackTransaction(transaction);
        CloseHandle(transaction);
    }
    if (error) return error;
    if (scope == SETTING_USER && root == HKEY_CURRENT_USER)
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 1000, NULL);
    return NULL;
}

struct journal {
    DWORD version, scope, type;
    wchar_t name[SETTING_CAP];
    struct setting_value before, after;
};

static const char *journal_path(bool create, wchar_t *dir, wchar_t *path)
{
    const char *error = brw_path(BRW_STATE, dir, create);
    if (error) return error;
    if (wcslen(dir) + 14 > BRW_PATH_CAP) return "state-directory";
    swprintf(path, BRW_PATH_CAP, L"%ls\\settings-v1", dir);
    return NULL;
}

static bool missing(DWORD err)
{
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

const char *brw_settings_pending(void)
{
    wchar_t dir[BRW_PATH_CAP], path[BRW_PATH_CAP];
    const char *error = journal_path(false, dir, path);
    if (error) return error;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return "recovery-required";
    return missing(GetLastError()) ? NULL : "settings-journal";
}

static const char *save(const wchar_t *dir, const wchar_t *path, const struct journal *j)
{
    wchar_t tmp[BRW_PATH_CAP];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        DWORD nonce;
        if (BCryptGenRandom(NULL, (BYTE *)&nonce, sizeof nonce, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return "settings-journal";
        /* Shorter than the checked final journal name. */
        swprintf(tmp, BRW_PATH_CAP, L"%ls\\~%08lx", dir, nonce);
        file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return "settings-journal";
    }
    if (file == INVALID_HANDLE_VALUE) return "settings-journal";
    DWORD bytes = 0;
    bool ok = WriteFile(file, j, sizeof *j, &bytes, NULL) && bytes == sizeof *j && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = false;
    if (ok && MoveFileExW(tmp, path, MOVEFILE_WRITE_THROUGH)) return NULL;
    DeleteFileW(tmp);
    return "settings-journal";
}

static bool valid_saved(const struct journal *j, const struct setting_value *v)
{
    return !v->type || ((v->type == j->type || (j->scope == SETTING_USER && v->type == REG_EXPAND_SZ)) &&
           (v->type == REG_DWORD || wmemchr(v->data.text, 0, SETTING_CAP)));
}

static const char *load(const wchar_t *path, struct journal *j)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return "settings-journal";
    BY_HANDLE_FILE_INFORMATION info;
    DWORD bytes = 0;
    bool ok = GetFileInformationByHandle(file, &info) &&
              !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
              !info.nFileSizeHigh && info.nFileSizeLow == sizeof *j &&
              ReadFile(file, j, sizeof *j, &bytes, NULL) && bytes == sizeof *j;
    CloseHandle(file);
    if (!ok || j->version != 1 || (j->scope != SETTING_REGISTRY && j->scope != SETTING_USER) ||
        !wmemchr(j->name, 0, SETTING_CAP) || allowed(j->scope, j->name, j->type) ||
        !valid_saved(j, &j->before) || !valid_saved(j, &j->after)) return "settings-journal";
    return NULL;
}

static HANDLE environment_lock(void)
{
    HANDLE lock = brw_user_mutex(L"UserPathInitialization");
    if (!lock) return NULL;
    DWORD wait = WaitForSingleObject(lock, 5000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return lock;
    CloseHandle(lock);
    return NULL;
}

const char *brw_settings_recover(bool preview, bool *pending, bool *preserved)
{
    wchar_t dir[BRW_PATH_CAP], path[BRW_PATH_CAP];
    const char *error = journal_path(false, dir, path);
    if (error) return error;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return missing(GetLastError()) ? NULL : "settings-journal";
    *pending = true;
    HANDLE lock = environment_lock();
    if (!lock) return "settings-busy";
    struct journal *j = calloc(1, sizeof *j);
    HKEY root = registry_root();
    error = j && root ? load(path, j) : "settings-unavailable";
    struct setting_value actual = {0};
    if (!error) {
        error = read_value(root, j->scope, j->name, &actual);
        if (error && (!strcmp(error, "value-type") || !strcmp(error, "value-too-long"))) {
            *preserved = true;
            error = NULL;
        } else if (!error && !equal(&actual, &j->before)) {
            if (!equal(&actual, &j->after)) *preserved = true;
            else if (!preview) error = write_value(root, j->scope, j->name, &j->after, &j->before);
        }
        if (!error && !preview && !DeleteFileW(path)) error = "settings-journal";
    }
    free(j);
    ReleaseMutex(lock);
    CloseHandle(lock);
    return error;
}

static const char *persist(HKEY root, const struct setting_request *r)
{
    wchar_t dir[BRW_PATH_CAP], path[BRW_PATH_CAP];
    const char *error = journal_path(true, dir, path);
    if (error) return error;
    struct journal *j = calloc(1, sizeof *j);
    if (!j) return "out-of-memory";
    j->version = 1;
    j->scope = r->setting.scope;
    j->type = native_type(r);
    wcscpy(j->name, r->setting.name);
    j->before = r->before;
    j->after = r->after;
    error = save(dir, path, j);
    free(j);
    if (error) return error;
    error = write_value(root, r->setting.scope, r->setting.name, &r->before, &r->after);
#ifdef BRW_TEST_INTERRUPTS
    wchar_t phase[32];
    DWORD n = GetEnvironmentVariableW(L"BRW_TEST_INTERRUPT", phase, 32);
    if (!error && n && n < 32 && !wcscmp(phase, L"settings")) ExitProcess(86);
#endif
    if (!DeleteFileW(path)) return "settings-journal";
    return error;
}

static const char *apply_locked(struct setting_request *r, HKEY root)
{
    const char *error = read_value(root, r->setting.scope, r->setting.name, &r->before);
    if (error) return error;
    if (r->before.type && r->before.type != native_type(r) &&
        !(r->setting.scope == SETTING_USER && r->before.type == REG_EXPAND_SZ)) return "value-type";
    if (r->setting.op == DIGBY_REGISTRY_READ) return NULL;
    if (r->setting.expect_missing && r->before.type) return "old-value-conflict";
    if (r->setting.expected) {
        struct setting_value expected = {0};
        error = brw_parse_value(r->before.type ? r->before.type : native_type(r), r->setting.expected, &expected);
        if (error) return error;
        if (!equal(&r->before, &expected)) return "old-value-conflict";
    }
    if (r->setting.op == DIGBY_PATH_ADD || r->setting.op == DIGBY_PATH_REMOVE) {
        wchar_t full[BRW_PATH_CAP];
        const wchar_t *p = r->setting.argument;
        if (!((wcslen(p) >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
              (p[0] == L'\\' && p[1] == L'\\'))) return "invalid-path-entry";
        DWORD size = GetFullPathNameW(p, BRW_PATH_CAP, full, NULL);
        if (!size || size >= BRW_PATH_CAP) return "invalid-path-entry";
        while (size > 3 && full[size-1] == L'\\') full[--size] = 0;
        r->after = r->before;
        if (!r->after.type) r->after.type = REG_SZ;
        bool changed;
        error = brw_edit_user_path(r->after.data.text, SETTING_CAP, r->after.type, full,
                                   r->setting.op == DIGBY_PATH_REMOVE, true, &changed);
        if (!changed) r->after = r->before;
    } else if (r->setting.argument) {
        error = brw_parse_value(r->before.type ? r->before.type : native_type(r), r->setting.argument, &r->after);
    }
    if (error || equal(&r->before, &r->after)) return error;
    if (r->setting.scope != SETTING_SESSION) {
        HKEY writable;
        if (open_target(root, r->setting.scope, CHECK_WRITE, NULL, &writable) != ERROR_SUCCESS)
            return "registry-write";
        if (writable) RegCloseKey(writable);
        wchar_t state[BRW_PATH_CAP];
        error = brw_writable_path(BRW_STATE, state);
        if (error) return error;
    }
    if (r->preview) return NULL;
    return r->setting.scope == SETTING_SESSION ?
        write_value(NULL, SETTING_SESSION, r->setting.name, &r->before, &r->after) : persist(root, r);
}

const char *brw_apply_setting(void *context, const struct digby_setting *step)
{
    struct setting_request *r = context;
    r->setting = *step;
    if (r->setting.scope == SETTING_REGISTRY && _wcsicmp(r->setting.key, BRW_SETTINGS_KEY)) return "unauthorized-key";
    const char *error = allowed(r->setting.scope, r->setting.name, native_type(r));
    if (error || r->setting.scope == SETTING_SESSION) return error ? error : apply_locked(r, NULL);
    HKEY root = registry_root();
    if (!root) return "registry-unavailable";
    if (step->op == DIGBY_REGISTRY_READ) return apply_locked(r, root);
    error = brw_begin_package_changes();
    if (error) return error;
    HANDLE lock = environment_lock();
    error = lock ? apply_locked(r, root) : "settings-busy";
    if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
    brw_end_package_changes();
    return error;
}
