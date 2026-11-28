#include "blueprint_cli.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

int brw_utf8_wcommand(int argc, wchar_t **argv, int (*command)(int, const char *const *))
{
    char **args = calloc((size_t)argc + 1, sizeof *args);
    int result = 2;
    if (!args) {
        fputs("brw: out-of-memory\n", stderr);
        return 1;
    }
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(args[i] = malloc((size_t)n)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, args[i], n, NULL, NULL)) {
            fputs("brw: invalid command argument\n", stderr);
            goto done;
        }
    }
    result = command(argc, (const char *const *)args);
done:
    for (int i = 0; i < argc; ++i) free(args[i]);
    free(args);
    return result;
}
