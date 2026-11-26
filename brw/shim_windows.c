#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "shim_windows.h"
#include "shim_script_windows.h"

int wmain(int argc, wchar_t **argv)
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

    wchar_t cwd[32768], command[32768], host[MAX_PATH];
    const wchar_t *application;
    DWORD n = GetCurrentDirectoryW(32768, cwd);
    const char *error = brw_entry_command(target, n && n < 32768 ? cwd : NULL, (size_t)argc,
        (const wchar_t *const *)argv, brw_command_tail(GetCommandLineW()), command, host, &application);
    if (error) {
        if (!strcmp(error, "batch-directory"))
            fputs("brw: cannot safely forward batch working directory to cmd.exe\n", stderr);
        else if (!strcmp(error, "batch-arguments"))
            fputs("brw: cannot safely forward batch path or argument containing shell syntax\n", stderr);
        else if (!strcmp(error, "child-command-too-long"))
            fputs("brw: command line too long\n", stderr);
        return 126;
    }
    STARTUPINFOW startup = {0};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    if (!CreateProcessW(application, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "brw: cannot launch target (Windows error %lu)\n", GetLastError());
        return 126;
    }
    CloseHandle(process.hThread);
    DWORD result = 126;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    ExitProcess(result);
}
