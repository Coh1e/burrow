#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings_windows.h"
#include "settings_backend_windows.h"
#include "source_windows.h"

static const char *parse(int argc, wchar_t **argv, struct setting_request *r)
{
    int i = 2;
    if (argc < 4) return "invalid-arguments";
    if (!wcscmp(argv[1], L"registry")) {
        if (argc < 6) return "invalid-arguments";
        r->setting.scope = SETTING_REGISTRY;
        const wchar_t *action = argv[i++];
        if (!wcscmp(action, L"get")) r->setting.op = DIGBY_REGISTRY_READ;
        else if (!wcscmp(action, L"set")) r->setting.op = DIGBY_REGISTRY_WRITE;
        else if (!wcscmp(action, L"remove")) r->setting.op = DIGBY_REGISTRY_DELETE;
        else return "invalid-action";
        r->setting.key = argv[i++];
        r->setting.name = argv[i++];
        const wchar_t *type = argv[i++];
        if (!_wcsicmp(type, L"REG_SZ")) r->setting.type = SETTING_STRING;
        else if (!_wcsicmp(type, L"REG_DWORD")) r->setting.type = SETTING_DWORD;
        else return "value-type";
    } else {
        if (!wcscmp(argv[i], L"session")) r->setting.scope = SETTING_SESSION;
        else if (!wcscmp(argv[i], L"user")) r->setting.scope = SETTING_USER;
        else return "invalid-scope";
        ++i;
        const wchar_t *action = argv[i++];
        r->setting.type = SETTING_STRING;
        if (!wcscmp(action, L"path-add") || !wcscmp(action, L"path-remove")) {
            r->setting.name = L"Path";
            r->setting.op = !wcscmp(action, L"path-add") ? DIGBY_PATH_ADD : DIGBY_PATH_REMOVE;
        } else {
            if (i == argc) return "invalid-arguments";
            r->setting.name = argv[i++];
            if (!wcscmp(action, L"set")) r->setting.op = DIGBY_ENV_SET;
            else if (!wcscmp(action, L"remove")) r->setting.op = DIGBY_ENV_REMOVE;
            else return "invalid-action";
        }
    }
    if (r->setting.op == DIGBY_ENV_SET || r->setting.op == DIGBY_REGISTRY_WRITE ||
        r->setting.op == DIGBY_PATH_ADD || r->setting.op == DIGBY_PATH_REMOVE) {
        if (i == argc) return "invalid-arguments";
        r->setting.argument = argv[i++];
    }
    while (i < argc) {
        const wchar_t *option = argv[i++];
        if (!wcscmp(option, L"--json")) r->json = true;
        else if (!wcscmp(option, L"--dry-run")) r->preview = true;
        else if (!wcscmp(option, L"--") && r->setting.scope == SETTING_SESSION && i < argc) {
            r->child = argv + i;
            r->child_count = argc - i;
            break;
        } else if (!wcscmp(option, L"--expect-missing") && !r->setting.expected && !r->setting.expect_missing)
            r->setting.expect_missing = true;
        else if (!wcscmp(option, L"--expect") && i < argc && !r->setting.expect_missing && !r->setting.expected)
            r->setting.expected = argv[i++];
        else return "invalid-arguments";
    }

    if (r->setting.scope == SETTING_SESSION && !r->child) return "child-required";
    return NULL;
}

/* Encode argv for CommandLineToArgvW/CRT. No shell interprets the arguments. */
static const char *run_child(const struct setting_request *r, DWORD *exit_code)
{
    wchar_t command[32768];
    size_t used = 0;
    for (int i = 0; i < r->child_count; ++i) {
        const wchar_t *p = r->child[i];
        if (used + 4 >= 32768) return "child-command-too-long";
        if (i) command[used++] = L' ';
        if (*p && !wcspbrk(p, L" \t\"")) {
            size_t length = wcslen(p);
            if (used + length >= 32768) return "child-command-too-long";
            wmemcpy(command + used, p, length);
            used += length;
            continue;
        }
        command[used++] = L'"';
        for (;;) {
            size_t slashes = wcsspn(p, L"\\");
            p += slashes;
            if (!*p || *p == L'"') slashes = 2 * slashes + (*p == L'"');
            if (used + slashes + 3 > 32768) return "child-command-too-long";
            wmemset(command + used, L'\\', slashes);
            used += slashes;
            if (!*p) break;
            command[used++] = *p++;
        }
        command[used++] = L'"';
    }
    command[used] = 0;
    STARTUPINFOW start = {0};
    start.cb = sizeof start;
    if (r->json) {
        start.dwFlags = STARTF_USESTDHANDLES;
        start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        start.hStdOutput = start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION child;
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &start, &child))
        return "child-launch";
    CloseHandle(child.hThread);
    bool ok = WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.hProcess, exit_code);
    CloseHandle(child.hProcess);
    return ok ? NULL : "child-wait";
}

static void print_value(const struct setting_value *v)
{
    if (!v->type) fputs("null", stdout);
    else if (v->type == REG_DWORD) printf("%lu", v->data.number);
    else brw_json_string(v->data.text);
}

int brw_settings_command(int argc, wchar_t **argv)
{
    struct setting_request *r = calloc(1, sizeof *r);
    if (!r) return 1;
    const char *error = parse(argc, argv, r);
    if (error) {
        for (int i = 2; i < argc && wcscmp(argv[i], L"--"); ++i)
            if (!wcscmp(argv[i], L"--json")) r->json = true;
    }
    if (!error) {
        struct digby_plan plan;
        error = digby_plan_setting(&r->setting, DIGBY_CAP_REGISTRY | DIGBY_CAP_USER_ENV | DIGBY_CAP_SESSION_ENV,
                                   &plan);
        if (!error) error = digby_execute(&plan, brw_apply_setting, r, NULL);
    }
    DWORD child_code = 0;
    if (!error && !r->preview && r->child) error = run_child(r, &child_code);
    const char *status = r->setting.op == DIGBY_REGISTRY_READ ? "ok" : r->preview ? "planned" : "applied";
    const struct setting_value *value = r->setting.op == DIGBY_REGISTRY_READ ? &r->before : &r->after;
    if (error) {
        if (r->json) printf("{\"status\":\"failed\",\"error\":\"%s\",\"step\":1}\n", error);
        else fprintf(stderr, "brw: %s\n", error);
    } else if (r->json) {
        printf("{\"status\":\"%s\",\"operation\":\"%s\",\"scope\":\"%s\",\"name\":",
               status, digby_operation_name(r->setting.op), r->setting.scope == SETTING_USER ? "user" :
               r->setting.scope == SETTING_SESSION ? "session" : "registry");
        brw_json_string(r->setting.name);
        if (r->setting.key) {
            fputs(",\"key\":", stdout); brw_json_string(r->setting.key);
        }
        printf(",\"type\":\"%s\",\"before\":", (value->type == REG_DWORD || r->setting.type == SETTING_DWORD) ?
               "REG_DWORD" : (value->type == REG_EXPAND_SZ ? "REG_EXPAND_SZ" : "REG_SZ"));
        print_value(&r->before);
        fputs(",\"value\":", stdout); print_value(value);
        if (r->child && !r->preview) printf(",\"childExitCode\":%lu", child_code);
        puts("}");
    } else if (!r->child || r->preview) {
        printf("%s %s ", status, digby_operation_name(r->setting.op));
        if (r->setting.key) { brw_json_string(r->setting.key); putchar(' '); }
        brw_json_string(r->setting.name); putchar(' '); print_value(value); putchar('\n');
    }
    free(r);
    return error ? 1 : (int)child_code;
}
