#include "blueprint_cli.h"
#include "archive_cli.h"
#include "download_cli.h"
#include "digby_download.h"
#include "json_utf8.h"
#include "blueprint.h"
#include "blueprint_lock.h"
#ifdef _WIN32
#include "ssh_windows.h"
#endif
#include <stdio.h>
#include <string.h>

static void field(const char *key, const char *value)
{
    if (value) {
        printf(",\"%s\":", key);
        quoted(value);
    }
}

static void print_state_status(enum bp_state_status status)
{
    static const char *const names[] = {NULL, "satisfied", "unmet", "changed", "check-at-execution"};
    field("status", names[status]);
    if (status == BP_DEFERRED) field("reason", "earlier-step-may-change-state");
}

static void print_ssh(const struct bp_ssh *ssh)
{
    field("target", ssh->target);
    field("command", ssh->command);
    printf(",\"port\":%u,\"timeoutSeconds\":%u", ssh->port, ssh->timeout_seconds);
    if (!ssh->reported) return;
    const struct digby_ssh_result *result = &ssh->result;
    field("status", digby_ssh_status_name(result->status));
    if (result->status == DIGBY_SSH_COMPLETED) printf(",\"remoteExit\":%d", result->remote_exit);
    else fputs(",\"remoteExit\":null", stdout);
    field("error", result->error);
    if (result->status == DIGBY_SSH_PLANNED) fputs(",\"pending\":[\"host-identity\"]", stdout);
}

static void print_step(const struct bp_plan *plan, const struct bp_step *s, bool preview)
{
    printf("{\"op\":");
    quoted(s->skip_op ? s->skip_op : bp_op_name(s->op));
    if (s->skip_platform) {
        field("status", "skipped");
        field("reason", "platform-mismatch");
        field("platform", s->skip_platform);
    }
    field("source", s->source);
    if (s->ssh) print_ssh(s->ssh);
    field("path", s->path);
    if (s->op == BP_DOWNLOAD) {
        field("url", s->url);
        field("sha256", s->sha256);
        if (preview) field("remoteStatus", "unverified");
    }
    if (s->pack) {
        field("format", s->pack->format);
        fputs(",\"inputs\":[", stdout);
        for (size_t i = 0; i < s->pack->count; ++i) {
            if (i) putchar(',');
            quoted(s->pack->inputs[i]);
        }
        putchar(']');
        if (s->pack->pending) field("status", "check-at-execution");
    }
    field("name", s->name);
    field("value", s->value);
    field("version", s->version);
    field("registry", s->registry);
    field("tool", s->tool);
    if (s->tool && s->package) field("version", s->package->version);
    field("entry", s->entry);
    if (s->op == BP_DEFAULT) print_state_status(s->entry_status);
    if (!s->skip_op && s->op == BP_RUN) {
        field("program", s->program);
        printf(",\"args\":[");
        for (size_t i = 0; i < s->argc; ++i) {
            if (i) putchar(',');
            if (s->payload && i == s->script_arg) {
                printf("{\"payload\":");
                quoted(s->payload->path);
                putchar('}');
            } else if (s->bindings[i].output) {
                const struct bp_output *output = &plan->outputs[s->bindings[i].output - 1];
                if (s->bindings[i].writer) printf("{\"outputFile\":");
                else {
                    printf("{\"from\":");
                    quoted(output->source);
                    printf(",\"output\":");
                }
                quoted(output->name);
                putchar('}');
            } else quoted(s->args[i]);
        }
        putchar(']');
    }
    if (s->state) {
        field("required", s->state->required);
        field("detected", s->state->detected);
        printf(",\"unmet\":[");
        bool first = true;
        for (unsigned code = 1; code < sizeof s->state->unmet; ++code) {
            if (s->state->unmet[code]) {
                printf("%s%u", first ? "" : ",", code);
                first = false;
            }
        }
        printf("],\"check\":");
        print_step(plan, &s->state->check, preview);
        if (s->state->has_change) {
            if (s->op == BP_HOST) printf(",\"onMismatch\":[\"repair\",\"recheck\"],\"repair\":");
            else printf(",\"change\":");
            print_step(plan, &s->state->change, preview);
        }
        print_state_status(s->state->status);
        if (s->state->has_exit_code) printf(",\"exitCode\":%lu", s->state->exit_code);
    }
    if (s->payload) {
        field("payload", s->payload->path);
        field("sha256", s->payload->digest);
    }
    if (s->op == BP_REPLACE) {
        field("from", s->from);
        field("to", s->to);
        printf(",\"count\":%zu", s->count);
    }
    putchar('}');
}

static void report(const struct bp_plan *plan, bool json, bool preview, bool generate,
                   const char *error, const struct bp_run_result *result)
{
    const char *status = error ? "failed" : generate ? "locked" : preview ? "planned" : "applied";
    const char *detail = error && result->failure_source ? result->failure_source : "";
    if (json) {
        printf("{\"status\":\"%s\",\"completed\":%zu,\"steps\":[", status, result->completed);
        for (size_t i = 0; i < plan->count; ++i) {
            if (i) putchar(',');
            print_step(plan, &plan->steps[i], preview);
        }
        putchar(']');
        if (plan->lock && plan->lock->count) {
            printf(",\"locked\":[");
            for (size_t i = 0; i < plan->lock->count; ++i) {
                const struct bp_package *row = &plan->lock->rows[i];
                if (i) putchar(',');
                printf("{\"name\":");
                quoted(row->name);
                field("platform", BRW_BP_PLATFORM "-" BRW_BP_ARCH);
                field("version", row->version);
                field("source", row->registry);
                field("sha256", row->digest);
                field("depends", row->depends);
                printf(",\"reused\":%s}", row->reused ? "true" : "false");
            }
            putchar(']');
        }
        field("error", error);
        if (*detail) field("detail", detail);
        if (result->command_started) printf(",\"commandStarted\":true");
        if (result->repair_completed) printf(",\"repairCompleted\":true");
        if (result->command_started || result->repair_completed || result->remote_result_unknown)
            printf(",\"mayHaveSideEffects\":true");
        puts("}");
    } else {
        printf("%s (%zu completed)\n", status, result->completed);
        const struct bp_step *failed = NULL;
        for (size_t i = 0; i < plan->count; ++i) {
            const struct bp_step *step = &plan->steps[i];
            if (error && step->source && !strcmp(step->source, detail)) failed = step;
            print_step(plan, step, preview);
            putchar('\n');
        }
        if (plan->lock) {
            for (size_t i = 0; i < plan->lock->count; ++i) {
                const struct bp_package *r = &plan->lock->rows[i];
                printf("%s %s/%s %s %s\n", BRW_BP_PLATFORM "-" BRW_BP_ARCH,
                       r->registry, r->name, r->version, r->digest);
            }
        }
        if (error) fprintf(stderr, "brw: %s %s\n", error, detail);
        if (failed && failed->op == BP_UNPACK && failed->payload)
            brw_unpack_diagnostic(failed->payload->path, failed->path, error);
        if (failed && failed->op == BP_DOWNLOAD) brw_download_diagnostic(failed->path, error);
        if (failed && failed->ssh && !strcmp(error, "invalid-ssh-request")) {
            if (!plan->ssh_options.identity_file)
                fputs("Supply the SSH identity file with --identity FILE.\n", stderr);
            if (!plan->ssh_options.known_hosts)
                fputs("Supply the trusted host key file with --known-hosts FILE.\n", stderr);
        }
        if (result->remote_result_unknown)
            fputs("Check the remote state before running this Blueprint again.\n", stderr);
        else if (result->repair_completed) fputs("The host repair completed and may have left side effects.\n", stderr);
        else if (result->command_started) fputs("The external command started and may have left side effects.\n", stderr);
    }
}

int brw_blueprint_command(int argc, const char *const *argv)
{
    const char *digest = NULL, *workspace = NULL;
    struct bp_ssh_options ssh = {0};
    bool json = false, dry_run = false;
    bool generate = argc >= 2 && !strcmp(argv[0], "lock");
    if (argc < 2 || (!generate && strcmp(argv[0], "preview") && strcmp(argv[0], "apply"))) goto usage;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--sha256") && !digest && i + 1 < argc) digest = argv[++i];
        else if (!strcmp(argv[i], "--workspace") && !workspace && i + 1 < argc) workspace = argv[++i];
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else if (!strcmp(argv[i], "--dry-run") && !dry_run) dry_run = true;
        else {
            if (generate) goto usage;
            const char **option = NULL;
            if (!strcmp(argv[i], "--allow-target")) option = &ssh.allowed_target;
            else if (!strcmp(argv[i], "--identity")) option = &ssh.identity_file;
            else if (!strcmp(argv[i], "--known-hosts")) option = &ssh.known_hosts;
            if (!option || *option || i + 1 == argc) goto usage;
            const char *value = argv[++i];
            if (!*value || !strncmp(value, "--", 2)) goto usage;
            *option = value;
        }
    }
    if ((generate && dry_run) || !digest || !workspace || !*workspace || !*argv[1]) goto usage;
    bool preview = dry_run || !strcmp(argv[0], "preview");
    struct bp_bundle bundle = {0};
    struct bp_plan plan = {0};
    plan.ssh_options = ssh;
    char detail[BP_DETAIL_CAP] = "";
    struct bp_run_result result = {.failure_source = detail};
    unsigned capabilities = (1u << BP_COPY) | (1u << BP_REPLACE) | (1u << BP_ENV) |
        (1u << BP_RUN) | (1u << BP_STATE) | (1u << BP_TOOL) | (1u << BP_DEFAULT) | (1u << BP_HOST) |
        (1u << BP_UNPACK) | (1u << BP_PACK);
    if (digby_download_available()) capabilities |= 1u << BP_DOWNLOAD;
#ifdef BRW_BP_NATIVE_INSTALL
    capabilities |= 1u << BP_INSTALL;
#endif
#ifdef _WIN32
    if (brw_windows_ssh_available()) {
        plan.ssh_backend = brw_windows_ssh_run;
        capabilities |= 1u << BP_SSH;
    }
#endif
    const char *error = bp_load(argv[1], digest, BRW_BP_PLATFORM, BRW_BP_ARCH,
                                capabilities, &bundle, detail);
    if (!error) error = bp_build(&bundle, &plan, detail);
    struct bp_lock lock = {0};
    plan.lock = &lock;
    if (!error) error = bp_prepare_lock(&plan, &bundle, argv[1], BRW_BP_PLATFORM "-" BRW_BP_ARCH,
                                        generate, &lock, detail);
    if (!error && !generate) {
        error = digby_run_blueprint(&plan, workspace, preview, &result);
    }
    report(&plan, json, preview, generate, error, &result);
    bp_lock_free(&lock);
    bp_plan_free(&plan);
    bp_bundle_free(&bundle);
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_BLUEPRINT_USAGE "\n", stderr);
    return 2;
}
