#include "blueprint_cli.h"
#include "archive_cli.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "source") && !strcmp(argv[2], "add"))
        return bp_source_add(argv[3], argv[4]);
    if (argc > 1 && !strcmp(argv[1], "blueprint"))
        return brw_blueprint_command(argc - 2, (const char *const *)(argv + 2));
    if (argc > 1 && !strcmp(argv[1], "unpack"))
        return brw_unpack_command(argc - 2, (const char *const *)(argv + 2));
    if (argc > 1 && !strcmp(argv[1], "pack"))
        return brw_pack_command(argc - 2, (const char *const *)(argv + 2));
    fputs("usage: " BRW_BLUEPRINT_USAGE "\n       " BRW_UNPACK_USAGE "\n       " BRW_PACK_USAGE "\n", stderr);
    return 2;
}
