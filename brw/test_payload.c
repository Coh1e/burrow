#include <stdio.h>
#include <string.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc == 7 && !wcscmp(argv[1], L"--probe") && !wcscmp(argv[2], L"") &&
        !wcscmp(argv[3], L"\u706b star") && !wcscmp(argv[4], L"a\"b") &&
        !wcscmp(argv[5], L"ends \\") && !wcscmp(argv[6], L"last")) {
        char line[32];
        FILE *marker = fopen("cwd.marker", "rb");
        if (!marker)
            return 4;
        fclose(marker);
        if (!fgets(line, sizeof line, stdin) || strcmp(line, "input\n"))
            return 5;
        puts("output");
        fputs("diagnostic\n", stderr);
        return 41;
    }
    if (argc != 2)
        return 2;
    wprintf(L"%ls\n", argv[1]);
    return 37;
}
