#ifndef BRW_SHIM_SCRIPT_WINDOWS_H
#define BRW_SHIM_SCRIPT_WINDOWS_H

#include <windows.h>
#include <wchar.h>

/* Windows program paths cannot contain a quote. Preserve all bytes following
   argv[0] for native and PowerShell targets. */
static inline const wchar_t *brw_command_tail(const wchar_t *command)
{
    if (*command == L'"') {
        ++command;
        while (*command && *command != L'"') ++command;
        if (*command) ++command;
    } else {
        while (*command && *command != L' ' && *command != L'\t') ++command;
    }
    return command;
}

/* target, cwd, tail and argv are borrowed. application is set to target or
   the caller's host buffer. Caller supplies command[32768] and host[MAX_PATH]. */
static inline const char *brw_entry_command(const wchar_t *target, const wchar_t *cwd,
    size_t argc, const wchar_t *const *argv, const wchar_t *tail,
    wchar_t command[32768], wchar_t host[MAX_PATH], const wchar_t **application)
{
    const wchar_t *ext = wcsrchr(target, L'.');
    int batch = ext && (!_wcsicmp(ext, L".cmd") || !_wcsicmp(ext, L".bat"));
    int powershell = ext && !_wcsicmp(ext, L".ps1");
    *application = target;
    if (batch || powershell) {
        const wchar_t *suffix = batch ? L"\\cmd.exe" : L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        UINT n = GetSystemDirectoryW(host, MAX_PATH);
        if (!n || n + wcslen(suffix) >= MAX_PATH) return "script-host";
        wcscpy(host + n, suffix);
        *application = host;
    }
    int written;
    if (batch) {
        if (!cwd || !wcsncmp(cwd, L"\\\\", 2) || !wcsncmp(cwd, L"//", 2)) return "batch-directory";
        /* cmd expands percent expressions even inside quotes. */
        const wchar_t *syntax = L"\"%!^&|<>()\r\n";
        if (wcspbrk(target, syntax)) return "batch-arguments";
        written = swprintf(command, 32768, L"\"%ls\" /d /v:off /s /c \"\"%ls\"", host, target);
        for (size_t i = 1; i < argc; ++i) {
            if (wcspbrk(argv[i], syntax)) return "batch-arguments";
            if (written < 0 || (size_t)written + wcslen(argv[i]) + 5 > 32768)
                return "child-command-too-long";
            written += swprintf(command + written, 32768 - written, L" \"%ls\"", argv[i]);
        }
        if (written < 0 || written >= 32766) return "child-command-too-long";
        wcscpy(command + written, L"\"");
    } else if (powershell) {
        written = swprintf(command, 32768, L"\"%ls\" -NoLogo -NoProfile -File \"%ls\"%ls",
                           host, target, tail);
    } else {
        written = swprintf(command, 32768, L"\"%ls\"%ls", target, tail);
    }
    return written < 0 || written >= 32767 ? "child-command-too-long" : NULL;
}
#endif
