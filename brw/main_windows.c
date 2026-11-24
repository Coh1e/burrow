#include "install_windows.h"
#include <stdbool.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    bool json = false;
    bool preview = false;
    const wchar_t *args[5];
    size_t count = 0;
    if (argc < 2 || wcscmp(argv[1], L"install"))
        goto usage;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--json"))
            json = true;
        else if (!wcscmp(argv[i], L"--dry-run"))
            preview = true;
        else if (count < 5)
            args[count++] = argv[i];
        else
            goto usage;
    }
    if (count != 5)
        goto usage;

    const struct local_package package = {args[0], args[1], args[2], args[3], args[4]};
    const struct digby_plan plan = digby_install_plan();
    const char *error = brw_validate_package(&package);
    bool invalid = error != NULL;
    size_t failed_step = 0;
    if (!error && !preview)
        error = brw_install_package(&package, &plan, &failed_step);

    if (json) {
        printf("{\"status\":\"%s\",\"operations\":[",
               error ? "failed" : preview ? "planned" : "installed");
        for (size_t i = 0; i < plan.count; ++i)
            printf("%s\"%s\"", i ? "," : "", digby_operation_name(plan.steps[i]));
        printf("]");
        if (error)
            printf(",\"error\":\"%s\",\"step\":%zu", error, invalid ? 0 : failed_step + 1);
        puts("}");
    } else if (error) {
        fprintf(stderr, "brw: %s\n", error);
    } else {
        for (size_t i = 0; i < plan.count; ++i)
            puts(digby_operation_name(plan.steps[i]));
    }
    return invalid ? 2 : error ? 1 : 0;

usage:
    fputs("usage: brw install NAME VERSION ARCHIVE SHA256 ENTRY [--dry-run] [--json]\n", stderr);
    return 2;
}
