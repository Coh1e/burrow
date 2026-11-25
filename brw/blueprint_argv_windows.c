#include "blueprint_cli.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

int brw_blueprint_wcommand(int argc, wchar_t **argv)
{
    char *args[8] = {0};
    int result = 2;
    if (argc > (int)(sizeof args / sizeof *args)) {
        fputs("usage: " BRW_BLUEPRINT_USAGE "\n", stderr);
        return result;
    }
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(args[i] = malloc((size_t)n)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, args[i], n, NULL, NULL)) {
            fputs("brw: invalid blueprint argument\n", stderr);
            goto done;
        }
    }
    result = brw_blueprint_command(argc, (const char *const *)args);
done:
    for (int i = 0; i < argc; ++i) free(args[i]);
    return result;
}
