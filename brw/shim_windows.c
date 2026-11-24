#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "shim_windows.h"

int wmain(void)
{
    wchar_t sidecar[BRW_PATH_CAP];
    DWORD size = GetModuleFileNameW(NULL, sidecar, BRW_PATH_CAP);
    if (!size || !brw_append_shim_suffix(sidecar, size))
        return 126;
    FILE *file = _wfopen(sidecar, L"rb");
    if (!file)
        return 126;
    wchar_t target[BRW_PATH_CAP];
    size_t length = fread(target, sizeof *target, BRW_PATH_CAP, file);
    int valid = !ferror(file) && feof(file) && length > 1 &&
                target[length - 1] == 0 && wcslen(target) == length - 1;
    fclose(file);
    if (!valid)
        return 126;

    const wchar_t *tail = GetCommandLineW();
    if (*tail == L'"') {
        ++tail;
        while (*tail && *tail != L'"')
            ++tail;
        if (*tail)
            ++tail;
    } else {
        while (*tail && *tail != L' ' && *tail != L'\t')
            ++tail;
    }
    wchar_t command[32768];
    if (length + wcslen(tail) + 2 >= 32768)
        return 126;
    swprintf(command, 32768, L"\"%ls\"%ls", target, tail);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    if (!CreateProcessW(target, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process))
        return 126;
    CloseHandle(process.hThread);
    DWORD result = 126;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    ExitProcess(result);
}
