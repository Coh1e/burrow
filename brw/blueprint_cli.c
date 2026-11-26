#include "blueprint_cli.h"
#include "blueprint.h"
#include "blueprint_lock.h"
#include <stdio.h>
#include <string.h>

static void quoted(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if (*p < 32) printf("\\u%04x", (unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}

static void field(const char *key, const char *value)
{
    if (value) {
        printf(",\"%s\":", key);
        quoted(value);
    }
}

static void print_step(const struct bp_plan *plan, const struct bp_step *s)
{
    printf("{\"op\":");
    quoted(s->skip_op ? s->skip_op : bp_op_name(s->op));
    if (s->skip_platform) {
        field("status", "skipped");
        field("reason", "platform-mismatch");
        field("platform", s->skip_platform);
    }
    field("source", s->source);
    field("path", s->path);
    field("name", s->name);
    field("value", s->value);
    field("version", s->version);
    field("registry", s->registry);
    field("tool", s->tool);
    if (s->tool && s->package) field("version", s->package->version);
    field("entry", s->entry);
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
        printf(",\"unmet\":[");
        bool first = true;
        for (unsigned code = 1; code < sizeof s->state->unmet; ++code) {
            if (s->state->unmet[code]) {
                printf("%s%u", first ? "" : ",", code);
                first = false;
            }
        }
        printf("],\"check\":");
        print_step(plan, &s->state->check);
        printf(",\"change\":");
        print_step(plan, &s->state->change);
        static const char *const statuses[] = {NULL, "satisfied", "unmet", "changed", "check-at-execution"};
        field("status", statuses[s->state->status]);
        if (s->state->status == BP_DEFERRED)
            field("reason", "earlier-step-may-change-state");
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
                   const char *error, const char *detail, size_t completed)
{
    const char *status = error ? "failed" : generate ? "locked" : preview ? "planned" : "applied";
    if (json) {
        printf("{\"status\":\"%s\",\"completed\":%zu,\"steps\":[", status, completed);
        for (size_t i = 0; i < plan->count; ++i) {
            if (i) putchar(',');
            print_step(plan, &plan->steps[i]);
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
        puts("}");
    } else {
        printf("%s (%zu completed)\n", status, completed);
        for (size_t i = 0; i < plan->count; ++i) {
            print_step(plan, &plan->steps[i]);
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
    }
}

int brw_blueprint_command(int argc, const char *const *argv)
{
    const char *digest = NULL, *workspace = NULL;
    bool json = false, dry_run = false;
    bool generate = argc >= 2 && !strcmp(argv[0], "lock");
    if (argc < 2 || (!generate && strcmp(argv[0], "preview") && strcmp(argv[0], "apply"))) goto usage;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--sha256") && !digest && i + 1 < argc) digest = argv[++i];
        else if (!strcmp(argv[i], "--workspace") && !workspace && i + 1 < argc) workspace = argv[++i];
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else if (!strcmp(argv[i], "--dry-run") && !dry_run) dry_run = true;
        else goto usage;
    }
    if ((generate && dry_run) || !digest || !workspace || !*workspace || !*argv[1]) goto usage;
    bool preview = dry_run || !strcmp(argv[0], "preview");
    struct bp_bundle bundle = {0};
    struct bp_plan plan = {0};
    char detail[BP_DETAIL_CAP] = "";
    const char *location = detail;
    unsigned capabilities = (1u << BP_COPY) | (1u << BP_REPLACE) | (1u << BP_ENV) |
        (1u << BP_RUN) | (1u << BP_STATE) | (1u << BP_TOOL) | (1u << BP_DEFAULT);
#ifdef BRW_BP_NATIVE_INSTALL
    capabilities |= 1u << BP_INSTALL;
#endif
    const char *error = bp_load(argv[1], digest, BRW_BP_PLATFORM, BRW_BP_ARCH,
                                capabilities, &bundle, detail);
    if (!error) error = bp_build(&bundle, &plan, detail);
    size_t completed = 0;
    struct bp_lock lock = {0};
    plan.lock = &lock;
    if (!error) error = bp_prepare_lock(&plan, &bundle, argv[1], BRW_BP_PLATFORM "-" BRW_BP_ARCH,
                                        generate, &lock, detail);
    if (!error && !generate) {
        error = digby_run_blueprint(&plan, workspace, preview, &completed, &location);
    }
    report(&plan, json, preview, generate, error, error ? location : "", completed);
    bp_lock_free(&lock);
    bp_plan_free(&plan);
    bp_bundle_free(&bundle);
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_BLUEPRINT_USAGE "\n", stderr);
    return 2;
}
