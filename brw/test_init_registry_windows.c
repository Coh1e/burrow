#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "init_windows.h"

static bool path_is(HKEY hive, DWORD expected_type, const wchar_t *expected)
{
    DWORD type, bytes = 0;
    wchar_t value[4096];
    bytes = sizeof value;
    return RegGetValueW(hive, L"Environment", L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                        &type, value, &bytes) == ERROR_SUCCESS &&
           type == expected_type && bytes == (wcslen(expected) + 1) * sizeof *value &&
           !wcscmp(value, expected);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3 || !SetEnvironmentVariableW(L"USERPROFILE", argv[2]))
        return 2;
    const wchar_t *overrides[] = {L"XDG_CONFIG_HOME", L"XDG_CACHE_HOME", L"XDG_DATA_HOME",
                                 L"XDG_STATE_HOME", L"BRW_BIN_DIR"};
    for (size_t i = 0; i < sizeof overrides / sizeof *overrides; ++i)
        SetEnvironmentVariableW(overrides[i], NULL);
    wchar_t bin[2048], expected[4096];
    if (swprintf(bin, 2048, L"%ls\\.local\\bin", argv[2]) < 0 ||
        swprintf(expected, 4096, L"%%SystemRoot%%\\other;%ls", bin) < 0)
        return 2;
    HKEY hive, environment;
    LSTATUS status = RegLoadAppKeyW(argv[1], &hive, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "RegLoadAppKeyW failed (%ld)\n", status);
        return 3;
    }
    int result = 0;
    if (brw_init_with_registry(hive, true, true) ||
        RegGetValueW(hive, L"Environment", L"Path", RRF_RT_ANY, NULL, NULL, &(DWORD){0}) != ERROR_FILE_NOT_FOUND ||
        GetFileAttributesW(bin) != INVALID_FILE_ATTRIBUTES) {
        result = 4;
    } else if (RegCreateKeyExW(hive, L"Environment", 0, NULL, 0, KEY_SET_VALUE, NULL,
                               &environment, NULL) != ERROR_SUCCESS) {
        result = 5;
    } else {
        const wchar_t original[] = L"%SystemRoot%\\other";
        status = RegSetValueExW(environment, L"Path", 0, REG_EXPAND_SZ,
                                (const BYTE *)original, sizeof original);
        RegCloseKey(environment);
        if (status != ERROR_SUCCESS ||
            brw_init_with_registry(hive, true, true) || !path_is(hive, REG_EXPAND_SZ, original) ||
            brw_init_with_registry(hive, true, false) || !path_is(hive, REG_EXPAND_SZ, expected) ||
            GetFileAttributesW(bin) == INVALID_FILE_ATTRIBUTES ||
            brw_init_with_registry(hive, true, false) || !path_is(hive, REG_EXPAND_SZ, expected))
            result = 6;
    }
    RegCloseKey(hive);
    if (result)
        return result;
    status = RegLoadAppKeyW(argv[1], &hive, KEY_READ, REG_PROCESS_APPKEY, 0);
    if (status != ERROR_SUCCESS)
        return 7;
    bool persisted = path_is(hive, REG_EXPAND_SZ, expected);
    RegCloseKey(hive);
    return persisted ? 0 : 8;
}
