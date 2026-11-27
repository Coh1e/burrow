#include "ssh_command_windows.h"
#include "digby.h"
#include "ssh_windows.h"
#include "source_windows.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct ssh_options {
    struct digby_ssh_request request;
    const char *allowed_target;
    bool preview, json;
};

static bool positive_number(const char *text, unsigned *value)
{
    unsigned number = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (number > (UINT_MAX - digit) / 10) return false;
        number = number * 10 + digit;
    }
    *value = number;
    return number != 0;
}

static bool parse(int argc, char **argv, struct ssh_options *options)
{
    struct digby_ssh_request *r = &options->request;
    if (argc < 3) return false;
    r->target = argv[0];
    int i = 1;
    while (i < argc && strcmp(argv[i], "--")) {
        const char *flag = argv[i++];
        if (!strcmp(flag, "--dry-run") && !options->preview) {
            options->preview = true;
        } else if (!strcmp(flag, "--json") && !options->json) {
            options->json = true;
        } else {
            if (i == argc || !*argv[i] || !strncmp(argv[i], "--", 2)) return false;
            const char *value = argv[i++];
            if (!strcmp(flag, "--identity") && !r->identity_file) r->identity_file = value;
            else if (!strcmp(flag, "--known-hosts") && !r->known_hosts) r->known_hosts = value;
            else if (!strcmp(flag, "--allow-target") && !options->allowed_target) options->allowed_target = value;
            else if (!strcmp(flag, "--port") && !r->port) {
                if (!positive_number(value, &r->port) || r->port > 65535) return false;
            } else if (!strcmp(flag, "--timeout") && !r->timeout_seconds) {
                if (!positive_number(value, &r->timeout_seconds)) return false;
            } else return false;
        }
    }
    if (argc - i != 2 || !r->identity_file || !r->known_hosts) return false;
    r->command = argv[i + 1];
    if (!r->port) r->port = 22;
    if (!r->timeout_seconds) r->timeout_seconds = 30;
    return true;
}

static char *utf8(const wchar_t *value)
{
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    char *text = size ? malloc((size_t)size) : NULL;
    if (text && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text, size, NULL, NULL)) {
        free(text);
        return NULL;
    }
    return text;
}

static int report(const struct digby_ssh_result *result, const struct ssh_options *options,
                  const wchar_t *target, const wchar_t *command)
{
    const char *operation = digby_operation_name(DIGBY_SSH);
    const char *status = "rejected";
    if (result->status == DIGBY_SSH_PLANNED) status = "planned";
    if (result->status == DIGBY_SSH_COMPLETED) status = "completed";
    if (result->status == DIGBY_SSH_UNKNOWN) status = "unknown";
    if (options->json) {
        printf("{\"operation\":\"%s\",\"status\":\"%s\",\"remoteExit\":", operation, status);
        if (result->status == DIGBY_SSH_COMPLETED) printf("%d", result->remote_exit);
        else fputs("null", stdout);
        if (result->error) printf(",\"error\":\"%s\"", result->error);
        if (result->status == DIGBY_SSH_PLANNED) {
            fputs(",\"target\":", stdout);
            brw_json_string(target);
            printf(",\"port\":%u,\"timeoutSeconds\":%u,\"command\":",
                   options->request.port, options->request.timeout_seconds);
            brw_json_string(command);
            fputs(",\"pending\":[\"host-identity\"]", stdout);
        }
        puts("}");
    } else if (result->status == DIGBY_SSH_PLANNED) {
        printf("Planned %s to %s:%u (timeout %u s). Host identity verification is pending.\n",
               operation, options->request.target, options->request.port, options->request.timeout_seconds);
        brw_json_string(command);
        putchar('\n');
    } else if (result->status == DIGBY_SSH_COMPLETED) {
        printf("SSH completed with remote exit %d.\n", result->remote_exit);
    } else {
        fprintf(stderr, "brw ssh: %s (%s)\n", status, result->error);
    }
    if (result->status == DIGBY_SSH_COMPLETED) return result->remote_exit;
    return result->status == DIGBY_SSH_PLANNED ? 0 : 1;
}

int brw_ssh_command(int argc, wchar_t **argv)
{
    /* At most five options taking a value, two switches, target, -- and command. */
    char *args[15] = {0};
    struct ssh_options options = {0};
    int code = 2;
    if ((size_t)argc > sizeof args / sizeof *args) goto usage;
    for (int i = 0; i < argc; ++i) {
        args[i] = utf8(argv[i]);
        if (!args[i]) goto usage;
    }
    if (!parse(argc, args, &options)) goto usage;
    struct digby_ssh_result result = brw_windows_ssh_run(&options.request, options.allowed_target, options.preview);
    code = report(&result, &options, argv[0], argv[argc - 1]);
    goto done;

usage:
    for (int i = 0; i < argc && wcscmp(argv[i], L"--"); ++i)
        if (!wcscmp(argv[i], L"--json")) options.json = true;
    if (options.json)
        puts("{\"operation\":\"ssh\",\"status\":\"rejected\",\"remoteExit\":null,\"error\":\"invalid-arguments\"}");
    else
        fputs("usage: " BRW_SSH_USAGE "\n", stderr);
done:
    for (size_t i = 0; i < sizeof args / sizeof *args; ++i) free(args[i]);
    return code;
}
