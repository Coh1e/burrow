#include "blueprint_cli.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "blueprint"))
        return brw_blueprint_command(argc - 2, (const char *const *)(argv + 2));
    fputs("usage: " BRW_BLUEPRINT_USAGE "\n", stderr);
    return 2;
}
