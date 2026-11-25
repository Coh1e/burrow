#include "blueprint_cli.h"
#include "blueprint.h"
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

static void print_step(const struct bp_step *s)
{
    printf("{\"op\":\"%s\"", bp_op_name(s->op));
    field("source", s->source);
    field("path", s->path);
    field("name", s->name);
    field("value", s->value);
    field("version", s->version);
    field("entry", s->entry);
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

static void report(const struct bp_plan *plan, bool json, bool preview,
                   const char *error, const char *detail, size_t completed)
{
    const char *status = error ? "failed" : preview ? "planned" : "applied";
    if (json) {
        printf("{\"status\":\"%s\",\"completed\":%zu,\"steps\":[", status, completed);
        for (size_t i = 0; i < plan->count; ++i) {
            if (i) putchar(',');
            print_step(&plan->steps[i]);
        }
        putchar(']');
        field("error", error);
        if (*detail) field("detail", detail);
        puts("}");
    } else {
        printf("%s (%zu completed)\n", status, completed);
        for (size_t i = 0; i < plan->count; ++i) {
            print_step(&plan->steps[i]);
            putchar('\n');
        }
        if (error) fprintf(stderr, "brw: %s %s\n", error, detail);
    }
}

int brw_blueprint_command(int argc, const char *const *argv)
{
    const char *digest = NULL, *workspace = NULL;
    bool json = false, dry_run = false;
    if (argc < 2 || (strcmp(argv[0], "preview") && strcmp(argv[0], "apply"))) goto usage;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--sha256") && !digest && i + 1 < argc) digest = argv[++i];
        else if (!strcmp(argv[i], "--workspace") && !workspace && i + 1 < argc) workspace = argv[++i];
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else if (!strcmp(argv[i], "--dry-run") && !dry_run) dry_run = true;
        else goto usage;
    }
    if (!digest || !workspace || !*workspace || !*argv[1]) goto usage;
    bool preview = dry_run || !strcmp(argv[0], "preview");
    struct bp_bundle bundle = {0};
    struct bp_plan plan = {0};
    char detail[BP_DETAIL_CAP] = "";
    const char *location = detail;
    unsigned capabilities = (1u << BP_COPY) | (1u << BP_REPLACE) | (1u << BP_ENV);
#ifdef BRW_BP_NATIVE_INSTALL
    capabilities |= 1u << BP_INSTALL;
#endif
    const char *error = bp_load(argv[1], digest, BRW_BP_PLATFORM, BRW_BP_ARCH,
                                capabilities, &bundle, detail);
    if (!error) error = bp_build(&bundle, &plan, detail);
    size_t completed = 0, failed_step = 0;
    if (!error) {
        error = digby_run_blueprint(&plan, workspace, preview, &completed, &failed_step);
        if (error) location = plan.steps[failed_step].source;
    }
    report(&plan, json, preview, error, error ? location : "", completed);
    bp_plan_free(&plan);
    bp_bundle_free(&bundle);
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_BLUEPRINT_USAGE "\n", stderr);
    return 2;
}
