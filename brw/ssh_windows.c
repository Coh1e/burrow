#include "ssh_windows.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "command_line_windows.h"

struct ssh_session {
    HANDLE process, input, reply;
    ULONGLONG deadline;
    char nonce[33];
    bool handler;
};

/* The CLI runs one explicit operation per process. */
static volatile LONG cancelled;

static BOOL WINAPI interrupt_ssh(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedExchange(&cancelled, 1);
    return TRUE;
}

static bool active(const struct ssh_session *s)
{
    return !InterlockedCompareExchange(&cancelled, 0, 0) && GetTickCount64() < s->deadline;
}

static void drop(HANDLE *handle)
{
    if (*handle && *handle != INVALID_HANDLE_VALUE) CloseHandle(*handle);
    *handle = NULL;
}

static wchar_t *wide(const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *out = n ? malloc((size_t)n * sizeof *out) : NULL;
    if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, n)) {
        free(out);
        return NULL;
    }
    return out;
}

/* OpenSSH parses each -o as a configuration line, then expands path tokens.
   Give it one quoted absolute filename and escape each literal percent. */
static wchar_t *file_option(const wchar_t *name, const char *text)
{
    wchar_t *input = wide(text);
    if (!input) return NULL;
    DWORD cap = GetFullPathNameW(input, 0, NULL, NULL);
    wchar_t *path = cap ? malloc((size_t)cap * sizeof *path) : NULL;
    DWORD n = path ? GetFullPathNameW(input, cap, path, NULL) : 0;
    free(input);
    wchar_t *option = NULL;
    if (n && n < cap && !wcspbrk(path, L"\"\r\n") && !wcsstr(path, L"$" L"{")) {
        size_t prefix = wcslen(name);
        option = malloc((prefix + 2 * (size_t)n + 4) * sizeof *option);
        if (option) {
            wchar_t *p = wmemcpy(option, name, prefix) + prefix;
            *p++ = L'=';
            *p++ = L'"';
            for (const wchar_t *c = path; *c; ++c) {
                if (*c == L'%') *p++ = L'%';
                *p++ = *c == L'\\' ? L'/' : *c;
            }
            *p++ = L'"';
            *p = 0;
        }
    }
    free(path);
    return option;
}

static bool client_path(wchar_t path[32768])
{
#ifdef BRW_TEST_INTERRUPTS
    DWORD test = GetEnvironmentVariableW(L"BRW_TEST_SSH_CLIENT", path, 32768);
    if (test) return test < 32768 && test >= 3 && path[1] == L':' &&
                     (path[2] == L'\\' || path[2] == L'/');
#endif
    const wchar_t suffix[] = L"\\OpenSSH\\ssh.exe";
    UINT n = GetSystemDirectoryW(path, 32768);
    if (!n || n >= 32768 - sizeof suffix / sizeof *suffix) return false;
    wcscpy(path + n, suffix);
    return true;
}

/* Only the three designated handles may cross into OpenSSH. The parent's pipe
   ends stay open across the READY / command / exit-status exchange. */
static bool start_client(struct ssh_session *s, const wchar_t *app, wchar_t *line)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in = NULL, out = NULL, err = NULL;
    STARTUPINFOEXW start = {0};
    PROCESS_INFORMATION child = {0};
    SIZE_T bytes = 0;
    bool initialized = false, started = false;
    if (!CreatePipe(&in, &s->input, &sa, 0) || !CreatePipe(&s->reply, &out, &sa, 0))
        goto done;
    if (!SetHandleInformation(s->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(s->reply, HANDLE_FLAG_INHERIT, 0)) goto done;
    DWORD mode = PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(s->input, &mode, NULL, NULL)) goto done;
    HANDLE diagnostic = GetStdHandle(STD_ERROR_HANDLE);
    if (diagnostic && diagnostic != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), diagnostic, GetCurrentProcess(), &err,
                             0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
    } else {
        err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (err == INVALID_HANDLE_VALUE) goto done;
    }
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    start.lpAttributeList = malloc(bytes);
    if (!start.lpAttributeList ||
        !InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &bytes)) goto done;
    initialized = true;
    HANDLE handles[] = {in, out, err};
    if (!UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof handles, NULL, NULL)) goto done;
    start.StartupInfo.cb = sizeof start;
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    start.StartupInfo.hStdInput = in;
    start.StartupInfo.hStdOutput = out;
    start.StartupInfo.hStdError = err;
    if (CreateProcessW(app, line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                       NULL, NULL, &start.StartupInfo, &child)) {
        s->process = child.hProcess;
        CloseHandle(child.hThread);
        started = true;
    }
done:
    if (initialized) DeleteProcThreadAttributeList(start.lpAttributeList);
    free(start.lpAttributeList);
    drop(&in);
    drop(&out);
    drop(&err);
    return started;
}

/* stdout contains two short protocol lines. Remote command stdout and stderr
   travel on SSH's separate stderr channel. Never parse either as a receipt. */
static bool read_line(struct ssh_session *s, char *line, size_t cap)
{
    size_t used = 0;
    while (active(s) && used + 1 < cap) {
        DWORD available = 0, got = 0;
        if (!PeekNamedPipe(s->reply, NULL, 0, NULL, &available, NULL)) return false;
        if (!available) {
            Sleep(10);
            continue;
        }
        char c;
        if (!ReadFile(s->reply, &c, 1, &got, NULL) || got != 1 || !c) return false;
        if (c == '\n') {
            if (used && line[used - 1] == '\r') --used;
            line[used] = 0;
            return true;
        }
        line[used++] = c;
    }
    return false;
}

static const char *connect_verified(void *context, const struct digby_ssh_request *request)
{
    struct ssh_session *s = context;
    s->deadline = GetTickCount64() + (ULONGLONG)request->timeout_seconds * 1000;
    InterlockedExchange(&cancelled, 0);
    s->handler = SetConsoleCtrlHandler(interrupt_ssh, TRUE) != 0;
    if (!s->handler) return "ssh-cancel-handler";
    UCHAR random[16];
    if (BCryptGenRandom(NULL, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return "ssh-session";
    for (size_t i = 0; i < sizeof random; ++i)
        snprintf(s->nonce + 2 * i, 3, "%02x", (unsigned)random[i]);
    wchar_t app[32768], line[32768], script[192], port[16];
    if (!client_path(app)) return "ssh-client";
    swprintf(port, sizeof port / sizeof *port, L"%u", request->port);
    swprintf(script, sizeof script / sizeof *script,
             L"printf 'brw-ready-%hs\\n'; sh -s >&2; rc=$?; printf 'brw-exit-%hs=%%d\\n' \"$rc\"",
             s->nonce, s->nonce);
    wchar_t *target = wide(request->target);
    wchar_t *identity = file_option(L"IdentityFile", request->identity_file);
    wchar_t *hosts = file_option(L"UserKnownHostsFile", request->known_hosts);
    const wchar_t *argv[] = {
        app, L"-F", L"none", L"-T", L"-p", port, L"-o", identity, L"-o", hosts,
        L"-o", L"StrictHostKeyChecking=yes", L"-o", L"GlobalKnownHostsFile=none",
        L"-o", L"UpdateHostKeys=no", L"-o", L"VerifyHostKeyDNS=no", L"-o", L"CheckHostIP=no",
        L"-o", L"PreferredAuthentications=publickey", L"-o", L"BatchMode=yes",
        L"-o", L"IdentitiesOnly=yes", L"-o", L"IdentityAgent=none", L"-o", L"CertificateFile=none",
        L"-o", L"ConnectionAttempts=1", L"--", target, script
    };
    const char *error = "invalid-ssh-request";
    if (target && identity && hosts) {
        error = encode_command_line(line, sizeof argv / sizeof *argv, argv);
        if (!error && (!active(s) || !start_client(s, app, line))) error = "ssh-client";
    }
    free(target);
    free(identity);
    free(hosts);
    if (error) return error;
    char expected[64], reply[64];
    snprintf(expected, sizeof expected, "brw-ready-%s", s->nonce);
    if (!read_line(s, reply, sizeof reply) || strcmp(expected, reply))
        return "ssh-session-unverified";
    return NULL;
}

static enum digby_ssh_dispatch send_once(void *context, const struct digby_ssh_request *request,
                                        int *remote_exit)
{
    struct ssh_session *s = context;
    const char *command = request->command;
    size_t length = strlen(command), sent = 0;
    while (sent < length && active(s)) {
        DWORD written = 0;
        size_t remaining = length - sent;
        DWORD chunk = remaining > 4096 ? 4096 : (DWORD)remaining;
        if (!WriteFile(s->input, command + sent, chunk, &written, NULL)) break;
        sent += written;
        if (!written) Sleep(10);
    }
    drop(&s->input); /* EOF lets the remote sh finish reading the submitted script. */
    if (!sent) return DIGBY_SSH_NOT_SENT;
    char line[64], prefix[48];
    int n = snprintf(prefix, sizeof prefix, "brw-exit-%s=", s->nonce);
    if (sent != length || !read_line(s, line, sizeof line) || strncmp(line, prefix, (size_t)n))
        return DIGBY_SSH_UNCONFIRMED;
    const char *number = line + n;
    size_t digits = strlen(number);
    if (!digits || digits > 3 || strspn(number, "0123456789") != digits || atoi(number) > 255)
        return DIGBY_SSH_UNCONFIRMED;
    *remote_exit = atoi(number);
    /* Allow ssh to drain the remote stderr stream and close cleanly. The exit
       receipt already proves completion even if the connection then drops. */
    while (active(s) && WaitForSingleObject(s->process, 10) == WAIT_TIMEOUT) {}
    return DIGBY_SSH_CONFIRMED;
}

static void release(void *context)
{
    struct ssh_session *s = context;
    if (s->process && WaitForSingleObject(s->process, 0) == WAIT_TIMEOUT)
        TerminateProcess(s->process, 1);
    drop(&s->process);
    drop(&s->input);
    drop(&s->reply);
    if (s->handler) SetConsoleCtrlHandler(interrupt_ssh, FALSE);
}

struct digby_ssh_result brw_windows_ssh_run(const struct digby_ssh_request *request,
                                           const char *allowed_target, bool preview)
{
    static const struct digby_ssh_transport transport = {connect_verified, send_once, release};
    struct ssh_session session = {0};
    return digby_ssh_run(request, allowed_target, preview, &transport, &session);
}
