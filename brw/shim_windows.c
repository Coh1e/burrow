#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "shim_windows.h"
#include "shim_script_windows.h"

/* A shim changes only its own environment before launching its sole child. */
static int prepend_path(const wchar_t *prefix)
{
    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (!need && GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 0;
    size_t n = wcslen(prefix);
    if (n + need + 1 > 32767) return 0;
    wchar_t *path = malloc((n + need + 1) * sizeof *path);
    if (!path) return 0;
    wcscpy(path, prefix);
    int valid = 1;
    if (need > 1) {
        path[n] = L';';
        valid = GetEnvironmentVariableW(L"PATH", path + n + 1, need) == need - 1;
    }
    int ok = valid && SetEnvironmentVariableW(L"PATH", path);
    free(path);
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t sidecar[BRW_PATH_CAP];
    DWORD size = GetModuleFileNameW(NULL, sidecar, BRW_PATH_CAP);
    if (!size || !brw_append_shim_suffix(sidecar, size))
        return 126;
    FILE *file = _wfopen(sidecar, L"rb");
    if (!file)
        return 126;
    wchar_t target[BRW_PATH_CAP], *prefix = NULL;
    int valid = brw_read_shim_target(file, target, &prefix);
    fclose(file);
    if (valid && prefix) valid = prepend_path(prefix);
    free(prefix);
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
