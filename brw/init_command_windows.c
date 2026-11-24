#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include "init_windows.h"
#include "paths_windows.h"
#include "source_windows.h"

HANDLE brw_user_mutex(const wchar_t *purpose)
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return NULL;
    union {
        TOKEN_USER user;
        BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } identity;
    DWORD size;
    wchar_t *sid = NULL;
    BOOL ok = GetTokenInformation(token, TokenUser, &identity, sizeof identity, &size);
    CloseHandle(token);
    if (!ok || !ConvertSidToStringSidW(identity.user.User.Sid, &sid))
        return NULL;
    wchar_t name[256];
    swprintf(name, 256, L"Global\\Burrow.%ls.%ls", purpose, sid);
    LocalFree(sid);
    return CreateMutexW(NULL, FALSE, name);
}

int brw_init_with_registry(HKEY root, bool json, bool preview)
{
    static const char *keys[] = {"config", "cache", "data", "state", "bin"};
    wchar_t paths[BRW_PATH_COUNT][BRW_PATH_CAP];
    const char *error = NULL;
    for (size_t i = 0; !error && i < BRW_PATH_COUNT; ++i)
        error = brw_writable_path((enum brw_path_kind)i, paths[i]);

    bool changed = false;
    HKEY key = NULL;
    HANDLE lock = NULL;
    if (!error && !preview) {
        lock = brw_user_mutex(L"UserPathInitialization");
        DWORD wait = lock ? WaitForSingleObject(lock, 5000) : WAIT_FAILED;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            if (lock)
                CloseHandle(lock);
            lock = NULL;
            error = "user-path-busy";
        }
    }
    if (!error) {
        LSTATUS result = preview ? RegOpenKeyExW(root, L"Environment", 0, KEY_QUERY_VALUE, &key)
            : RegCreateKeyExW(root, L"Environment", 0, NULL, 0,
                              KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
        if (result == ERROR_SUCCESS || (preview && result == ERROR_FILE_NOT_FOUND)) {
            error = brw_initialize_path(key, paths[BRW_BIN], true, &changed);
            if (!error && !preview)
                error = brw_path(BRW_BIN, paths[BRW_BIN], true);
            if (!error && !preview && changed)
                error = brw_initialize_path(key, paths[BRW_BIN], false, &changed);
        } else {
            error = "user-path-open";
        }
    }
    if (key)
        RegCloseKey(key);
    if (lock) {
        ReleaseMutex(lock);
        CloseHandle(lock);
    }
    if (!error && !preview && changed && root == HKEY_CURRENT_USER)
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                            SMTO_ABORTIFHUNG, 1000, NULL);

    if (json) {
        if (error) {
            printf("{\"status\":\"failed\",\"error\":\"%s\",\"step\":0}\n", error);
        } else {
            printf("{\"status\":\"%s\",\"pathChanged\":%s,\"paths\":{",
                   preview ? "planned" : "initialized", changed ? "true" : "false");
            for (size_t i = 0; i < BRW_PATH_COUNT; ++i) {
                printf("%s\"%s\":", i ? "," : "", keys[i]);
                brw_json_string(paths[i]);
            }
            puts("}}");
        }
    } else if (error) {
        fprintf(stderr, "brw: %s\n", error);
    } else {
        for (size_t i = 0; i < BRW_PATH_COUNT; ++i)
            printf("%s %ls\n", keys[i], paths[i]);
        puts(changed ? (preview ? "User PATH would be updated." : "User PATH updated.")
                     : "User PATH already includes the entry directory.");
    }
    return error ? 1 : 0;
}

int brw_init(bool json, bool preview)
{
    return brw_init_with_registry(HKEY_CURRENT_USER, json, preview);
}
