#include "archive_cli.h"
#include "blueprint_bundle_io.h"
#include "digby_archive.h"
#include "json_utf8.h"
#include "verified_file.h"
#include <stdlib.h>
#include <string.h>

void brw_unpack_diagnostic(const char *payload, const char *destination, const char *error)
{
    const char *hint;
    if (!strcmp(error, "destination-exists")) hint = "Choose a new directory for the output.";
    else if (!strcmp(error, "invalid-digest") || !strcmp(error, "file-digest-mismatch"))
        hint = "Check the SHA-256 against the trusted source.";
    else if (!strcmp(error, "unsafe-parent") || !strcmp(error, "unsafe-destination"))
        hint = "Choose a relative output path under an existing workspace directory.";
    else if (!strcmp(error, "archive-data") || !strcmp(error, "archive-path") ||
             !strcmp(error, "archive-path-conflict"))
        hint = "Check the archive contents and the names of its entries.";
    else if (!strcmp(error, "archive-limit") || !strcmp(error, "bundle-limit"))
        hint = "Use a smaller archive within the documented limits.";
    else if (!strcmp(error, "publish-directory"))
        hint = "Check that the workspace supports an exclusive directory rename.";
    else if (!strcmp(error, "cleanup-stage") || !strcmp(error, "stage-changed"))
        hint = "Inspect the staging directory before retrying.";
    else hint = "Check the input file and workspace permissions.";
    fprintf(stderr, "brw unpack %s -> %s: %s. %s\n", payload, destination, error, hint);
}

int brw_pack_command(int argc, const char *const *argv)
{
    const char **inputs = NULL;
    if (argc < 2) goto usage;
    inputs = calloc((size_t)argc, sizeof *inputs);
    if (!inputs) { fputs("brw pack: out-of-memory\n", stderr); return 1; }
    struct digby_pack_plan plan = {.destination = argv[0], .inputs = inputs};
    bool preview = false, json = false, operands_only = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (operands_only || strncmp(arg, "--", 2)) inputs[plan.count++] = arg;
        else if (!strcmp(arg, "--")) operands_only = true;
        else if (!strcmp(arg, "--format") && !plan.format && i + 1 < argc) plan.format = argv[++i];
        else if (!strcmp(arg, "--workspace") && !plan.workspace && i + 1 < argc) plan.workspace = argv[++i];
        else if (!strcmp(arg, "--dry-run") && !preview) preview = true;
        else if (!strcmp(arg, "--json") && !json) json = true;
        else goto usage;
    }
    if (!plan.count || !plan.format || !plan.workspace || !*plan.workspace) goto usage;
    for (int i = 0; i < argc; ++i) {
        if (bp_valid_utf8(argv[i], strlen(argv[i]))) continue;
        if (json) puts("{\"operation\":\"pack\",\"status\":\"failed\",\"error\":\"invalid-utf8\"}");
        else fputs("brw pack: invalid-utf8. Supply UTF-8 paths and format.\n", stderr);
        free(inputs);
        return 1;
    }
    const char *error = digby_archive_pack(&plan, preview);
    const char *status = error ? "failed" : preview ? "planned" : "applied";
    if (json) {
        printf("{\"operation\":\"pack\",\"status\":\"%s\",\"format\":", status);
        quoted(plan.format);
        fputs(",\"path\":", stdout);
        quoted(plan.destination);
        fputs(",\"inputs\":[", stdout);
        for (size_t i = 0; i < plan.count; ++i) {
            if (i) putchar(',');
            quoted(inputs[i]);
        }
        putchar(']');
        if (error) { fputs(",\"error\":", stdout); quoted(error); }
        puts("}");
    } else if (error) {
        fprintf(stderr, "brw pack %s: %s. Check the format, input files and new output path.\n",
                plan.destination, error);
    } else {
        printf("%s pack %s (%s)\n", status, plan.destination, plan.format);
        for (size_t i = 0; i < plan.count; ++i) printf("  %s\n", inputs[i]);
    }
    free(inputs);
    return error ? 1 : 0;
usage:
    free(inputs);
    fputs("usage: " BRW_PACK_USAGE "\n", stderr);
    return 2;
}

int brw_unpack_command(int argc, const char *const *argv)
{
    const char *digest = NULL, *workspace = NULL;
    bool preview = false, json = false;
    if (argc < 2) goto usage;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--sha256") && !digest && i + 1 < argc) digest = argv[++i];
        else if (!strcmp(argv[i], "--workspace") && !workspace && i + 1 < argc) workspace = argv[++i];
        else if (!strcmp(argv[i], "--dry-run") && !preview) preview = true;
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else goto usage;
    }
    if (!digest || !workspace || !*workspace || !*argv[0]) goto usage;
    if (!bp_valid_utf8(argv[0], strlen(argv[0])) || !bp_valid_utf8(argv[1], strlen(argv[1])) ||
        !bp_valid_utf8(workspace, strlen(workspace)) || !bp_valid_utf8(digest, strlen(digest))) {
        if (json) puts("{\"operation\":\"unpack\",\"status\":\"failed\",\"error\":\"invalid-utf8\"}");
        else fputs("brw unpack: invalid-utf8. Supply UTF-8 paths and a hexadecimal digest.\n", stderr);
        return 1;
    }

    unsigned char *data = NULL;
    size_t size = 0;
    const char *error = brw_valid_sha256(digest) ? NULL : "invalid-digest";
    if (!error) error = brw_read_local_file(argv[0], BRW_PAYLOAD_LIMIT, &data, &size);
    if (!error && !brw_sha256_matches(data, size, digest)) error = "file-digest-mismatch";
    if (!error) {
        const struct digby_archive_plan plan = {data, size, workspace, argv[1]};
        error = digby_archive_extract(&plan, preview);
    }
    free(data);
    const char *status = error ? "failed" : preview ? "planned" : "applied";
    if (json) {
        printf("{\"operation\":\"unpack\",\"status\":\"%s\",\"payload\":", status);
        quoted(argv[0]);
        fputs(",\"path\":", stdout);
        quoted(argv[1]);
        fputs(",\"sha256\":", stdout);
        quoted(digest);
        if (error) { fputs(",\"error\":", stdout); quoted(error); }
        puts("}");
    } else if (error) brw_unpack_diagnostic(argv[0], argv[1], error);
    else printf("%s unpack %s (SHA-256 %s) -> %s\n", status, argv[0], digest, argv[1]);
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_UNPACK_USAGE "\n", stderr);
    return 2;
}
