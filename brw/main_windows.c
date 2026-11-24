#include "install_windows.h"
#include <stdbool.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    bool json = false;
    bool preview = false;
    const wchar_t *args[5];
    size_t count = 0;
    static const struct {
        const wchar_t *verb;
        enum digby_action action;
        size_t operands;
        const char *status;
    } commands[] = {
        {L"install", DIGBY_INSTALL, 5, "installed"},
        {L"update", DIGBY_UPDATE, 5, "updated"},
        {L"rollback", DIGBY_ROLLBACK, 2, "rolled-back"},
        {L"uninstall", DIGBY_UNINSTALL, 1, "uninstalled"}
    };
    if (argc < 2)
        goto usage;
    size_t command = 0;
    while (command < sizeof commands / sizeof *commands &&
           wcscmp(argv[1], commands[command].verb))
        ++command;
    if (command == sizeof commands / sizeof *commands)
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
    if (count != commands[command].operands)
        goto usage;

    const enum digby_action action = commands[command].action;
    struct local_package package = {0};
    package.name = args[0];
    if (count >= 2)
        package.version = args[1];
    if (count == 5) {
        package.archive = args[2];
        package.digest = args[3];
        package.entry = args[4];
    }
    const struct digby_plan plan = digby_lifecycle_plan(action);
    const char *error = brw_validate_lifecycle(action, &package);
    bool invalid = error != NULL;
    size_t failed_step = 0;
    if (!error && !preview)
        error = brw_run_lifecycle(action, &package, &plan, &failed_step);

    if (json) {
        printf("{\"status\":\"%s\",\"operations\":[",
               error ? "failed" : preview ? "planned" : commands[command].status);
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
    fputs("usage: brw install|update NAME VERSION ARCHIVE SHA256 ENTRY [--dry-run] [--json]\n"
          "       brw rollback NAME VERSION [--dry-run] [--json]\n"
          "       brw uninstall NAME [--dry-run] [--json]\n", stderr);
    return 2;
}
