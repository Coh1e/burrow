/* Local OpenSSH stand-in. Records process starts and the bytes sent on stdin. */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int record(const wchar_t *name, const void *bytes, size_t length)
{
    const wchar_t *path = _wgetenv(name);
    FILE *file = path ? _wfopen(path, L"ab") : NULL;
    if (!file) return 0;
    size_t written = fwrite(bytes, 1, length, file);
    return fclose(file) == 0 && written == length;
}

int wmain(int argc, wchar_t **argv)
{
    const char *mode = getenv("BRW_FIXTURE_MODE");
    if (argc < 2 || !mode || !record(L"BRW_FIXTURE_CONNECT", "connect\n", 8)) return 2;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    for (int i = 1; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
        char *arg = size > 0 ? malloc((size_t)size) : NULL;
        if (!arg) return 2;
        int ok = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, size, NULL, NULL) &&
                 record(L"BRW_FIXTURE_ARGS", arg, (size_t)size - 1) &&
                 record(L"BRW_FIXTURE_ARGS", "\n", 1);
        free(arg);
        if (!ok) return 2;
    }
    if (!strcmp(mode, "identity-mismatch")) {
        DWORD pending = 0;
        if (PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), NULL, 0, NULL, &pending, NULL) && pending)
            record(L"BRW_FIXTURE_SEND", "early command", 13);
        fputs("Host key verification failed.\n", stderr);
        return 255;
    }
    const wchar_t *tag = wcsstr(argv[argc - 1], L"brw-ready-");
    if (!tag) return 2;
    tag += wcslen(L"brw-ready-");
    char nonce[33] = {0};
    for (size_t i = 0; i < sizeof nonce - 1; ++i) {
        if (!tag[i] || !wcschr(L"0123456789abcdefABCDEF", tag[i])) return 2;
        nonce[i] = (char)tag[i];
    }
    printf("brw-ready-%s\n", nonce);
    fflush(stdout);
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, stdin)) > 0)
        if (!record(L"BRW_FIXTURE_SEND", buffer, n)) return 2;
    if (ferror(stdin)) return 2;
    if (!strcmp(mode, "disconnect")) return 255;
    if (!strcmp(mode, "timeout")) { Sleep(20000); return 1; }
    if (!strcmp(mode, "wrong-receipt")) { puts("brw-exit-unrelated=0"); return 0; }
    if (strcmp(mode, "success") && strcmp(mode, "long-command") && strcmp(mode, "exit255")) return 2;
    fputs("remote output\n", stderr);
    printf("brw-exit-%s=%d\n", nonce, !strcmp(mode, "exit255") ? 255 : 0);
    return 0;
}
