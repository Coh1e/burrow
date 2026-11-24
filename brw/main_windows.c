#include "install_windows.h"
#include "source_windows.h"
#include "paths_windows.h"
#include <stdio.h>
#include <string.h>

int brw_report_plan(bool json, bool preview, const char *status,
                    const struct digby_plan *plan, const char *error, size_t step)
{
    if (json) {
        printf("{\"status\":\"%s\",\"operations\":[",
               error ? "failed" : preview ? "planned" : status);
        for (size_t i = 0; i < plan->count; ++i)
            printf("%s\"%s\"", i ? "," : "", digby_operation_name(plan->steps[i]));
        putchar(']');
        if (preview && plan->steps[0] == DIGBY_DOWNLOAD)
            fputs(",\"pending\":[\"payload-availability\",\"sha256-verification\"]", stdout);
        if (error)
            printf(",\"error\":\"%s\",\"step\":%zu", error, step);
        puts("}");
    } else if (error) {
        fprintf(stderr, "brw: %s\n", error);
    } else {
        for (size_t i = 0; i < plan->count; ++i)
            puts(digby_operation_name(plan->steps[i]));
        if (preview && plan->steps[0] == DIGBY_DOWNLOAD)
            puts("Pending checks include payload availability and SHA-256 verification.");
    }
    return error ? 1 : 0;
}

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
    if (!wcscmp(argv[1], L"init") && !count)
        return brw_init(json, preview);
    if (!wcscmp(argv[1], L"source") || !wcscmp(argv[1], L"search") ||
        !wcscmp(argv[1], L"show") || !wcscmp(argv[1], L"list") ||
        (!wcscmp(argv[1], L"install") && count == 1))
        return brw_source_command(argv[1], count, args, json, preview);
    if (command == sizeof commands / sizeof *commands || count != commands[command].operands)
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
    bool locked = false;
    if (!error && !preview) {
        error = brw_begin_package_changes();
        locked = !error;
    }
    wchar_t dependent[BRW_PACKAGE_NAME_CAP];
    if (!error && action == DIGBY_UNINSTALL)
        error = brw_check_uninstall(package.name, dependent);
    if (error && action == DIGBY_UNINSTALL && !strcmp(error, "package-required")) {
        if (json) {
            fputs("{\"status\":\"failed\",\"operations\":[\"remove\"],"
                  "\"error\":\"package-required\",\"step\":0,\"dependent\":", stdout);
            brw_json_string(dependent);
            puts("}");
        } else {
            fprintf(stderr, "brw: package-required (%ls)\n", dependent);
        }
        if (locked)
            brw_end_package_changes();
        return 1;
    }
    if (!error && !preview)
        error = brw_run_lifecycle(action, &package, &plan, &failed_step);
    if (locked)
        brw_end_package_changes();

    int result = brw_report_plan(json, preview, commands[command].status,
                                 &plan, error, invalid ? 0 : failed_step + 1);
    return invalid ? 2 : result;

usage:
    fputs("usage: brw install|update NAME VERSION ARCHIVE SHA256 ENTRY [--dry-run] [--json]\n"
          "       brw rollback NAME VERSION [--dry-run] [--json]\n"
          "       brw source add NAME CATALOG | source remove NAME | source list\n"
          "       brw search [QUERY] | show [SOURCE/]NAME | list\n"
          "       brw install [SOURCE/]NAME [--dry-run] [--json]\n"
          "       brw uninstall NAME [--dry-run] [--json]\n"
          "       brw init [--dry-run] [--json]\n", stderr);
    return 2;
}
