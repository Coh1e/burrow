#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
        return 1;
    FILE *marker = _wfopen(argv[1], L"w");
    if (!marker)
        return 2;
    fputs(GetConsoleWindow() ? "console" : "gui", marker);
    return fclose(marker) ? 3 : 0;
}
