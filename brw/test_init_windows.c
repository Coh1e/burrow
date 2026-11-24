#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include "init_windows.h"

int main(void)
{
    wchar_t value[512];
    bool changed;
    const wchar_t bin[] = L"C:\\Burrow User\\Bin";
    const wchar_t original[] = L"%BRW_PATH_TEST%\\Other;C:\\existing";
    const wchar_t expected[] = L"%BRW_PATH_TEST%\\Other;C:\\existing;C:\\Burrow User\\Bin";
    const wchar_t equivalent[] = L"\"%BRW_PATH_TEST%\\bin\\\";C:\\existing";
    if (!SetEnvironmentVariableW(L"BRW_PATH_TEST", L"C:\\Burrow User"))
        return 1;
    wcscpy(value, original);
    if (brw_append_user_path(value, 512, REG_EXPAND_SZ, bin, false, &changed) ||
        !changed || wcscmp(value, original))
        return 2;
    if (brw_append_user_path(value, 512, REG_EXPAND_SZ, bin, true, &changed) ||
        !changed || wcscmp(value, expected) ||
        brw_append_user_path(value, 512, REG_EXPAND_SZ, bin, true, &changed) ||
        changed || wcscmp(value, expected))
        return 3;
    wcscpy(value, equivalent);
    if (brw_append_user_path(value, 512, REG_EXPAND_SZ, bin, true, &changed) ||
        changed || wcscmp(value, equivalent))
        return 4;
    if (brw_append_user_path(value, 512, REG_SZ, bin, true, &changed) || !changed)
        return 5;
    wcscpy(value, L"\"C:\\other;C:\\Burrow User\\Bin;folder\"");
    if (brw_append_user_path(value, 512, REG_SZ, bin, true, &changed) || !changed)
        return 6;
    value[0] = 0;
    const char *error = brw_append_user_path(value, 2, REG_SZ, bin, true, &changed);
    if (!error || strcmp(error, "user-path-too-long") || *value)
        return 7;
    error = brw_append_user_path(value, 512, REG_BINARY, bin, true, &changed);
    if (!error || strcmp(error, "user-path-read") || *value)
        return 8;
    if (brw_append_user_path(value, 512, REG_EXPAND_SZ, bin, true, &changed) ||
        !changed || wcscmp(value, bin))
        return 9;
    return 0;
}
