#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "shim_windows.h"

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
    const wchar_t *ext = wcsrchr(target, L'.');
    const wchar_t *application = target;
    wchar_t host[MAX_PATH];
    int batch = ext && (!_wcsicmp(ext, L".cmd") || !_wcsicmp(ext, L".bat"));
    int powershell = ext && !_wcsicmp(ext, L".ps1");
    int written;
    if (batch || powershell) {
        const wchar_t *suffix = batch ? L"\\cmd.exe" :
            L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        UINT n = GetSystemDirectoryW(host, MAX_PATH);
        if (!n || n + wcslen(suffix) >= MAX_PATH)
            return 126;
        wcscpy(host + n, suffix);
        application = host;
    }
    if (batch) {
        wchar_t cwd[32768];
        DWORD n = GetCurrentDirectoryW(32768, cwd);
        if (!n || n >= 32768 || !wcsncmp(cwd, L"\\\\", 2)) {
            fputs("brw: cannot safely forward batch working directory to cmd.exe\n", stderr);
            return 126;
        }
        /* cmd expands percent expressions even inside quotes. Reject shell syntax
           in the target and arguments before constructing its /s /c command. */
        const wchar_t *syntax = L"\"%!^&|<>()\r\n";
        if (wcspbrk(target, syntax))
            goto unsafe_batch;
        written = swprintf(command, 32768, L"\"%ls\" /d /v:off /s /c \"\"%ls\"", host, target);
        for (int i = 1; i < argc; ++i) {
            if (wcspbrk(argv[i], syntax))
                goto unsafe_batch;
            if (written < 0 || (size_t)written + wcslen(argv[i]) + 5 > 32768)
                goto too_long;
            written += swprintf(command + written, 32768 - written, L" \"%ls\"", argv[i]);
        }
        if (written < 0 || written >= 32766)
            goto too_long;
        wcscpy(command + written, L"\"");
    } else if (powershell) {
        written = swprintf(command, 32768, L"\"%ls\" -NoLogo -NoProfile -File \"%ls\"%ls",
                           host, target, tail);
    } else {
        written = swprintf(command, 32768, L"\"%ls\"%ls", target, tail);
    }
    if (written < 0 || written >= 32767)
        goto too_long;
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

unsafe_batch:
    fputs("brw: cannot safely forward batch path or argument containing shell syntax\n", stderr);
    return 126;
too_long:
    fputs("brw: command line too long\n", stderr);
    return 126;
}
