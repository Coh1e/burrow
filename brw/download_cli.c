#include "download_cli.h"
#include "digby_download.h"
#include "json_utf8.h"
#include "verified_file.h"
#include <string.h>

void brw_download_diagnostic(const char *destination, const char *error)
{
    const char *hint;
    if (!strcmp(error, "download-url")) hint = "Supply an HTTP(S) URL without credentials or a fragment.";
    else if (!strcmp(error, "missing-capability")) hint = "Check that curl is executable on PATH on Linux or macOS.";
    else if (!strcmp(error, "file-digest-mismatch") || !strcmp(error, "invalid-digest"))
        hint = "Check the expected SHA-256 against the trusted source.";
    else if (!strcmp(error, "destination-exists")) hint = "Choose a new workspace file.";
    else if (!strcmp(error, "unsafe-destination") || !strcmp(error, "unsafe-parent") ||
             !strcmp(error, "invalid-workspace")) hint = "Choose a relative path under an existing workspace directory.";
    else if (!strcmp(error, "download-limit")) hint = "Use a file of at most 64 MiB.";
    else if (!strcmp(error, "download-failed")) hint = "Check the URL and availability of the complete file.";
    else if (!strcmp(error, "cancelled")) hint = "Run the download again when ready.";
    else if (!strcmp(error, "cleanup-stage") || !strcmp(error, "stage-changed"))
        hint = "Inspect the staging and destination paths before retrying.";
    else hint = "Check workspace permissions and available space.";
    fprintf(stderr, "brw download %s: %s. %s\n", destination ? destination : "(missing path)", error, hint);
}

int brw_download_command(int argc, const char *const *argv)
{
    if (argc < 2) goto usage;
    struct digby_download_plan plan = {.url = argv[0], .destination = argv[1]};
    bool preview = false, json = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--sha256") && !plan.sha256 && i + 1 < argc) plan.sha256 = argv[++i];
        else if (!strcmp(argv[i], "--workspace") && !plan.workspace && i + 1 < argc) plan.workspace = argv[++i];
        else if (!strcmp(argv[i], "--dry-run") && !preview) preview = true;
        else if (!strcmp(argv[i], "--json") && !json) json = true;
        else goto usage;
    }
    if (!plan.sha256 || !plan.workspace) goto usage;
    for (int i = 0; i < argc; ++i) {
        if (bp_valid_utf8(argv[i], strlen(argv[i]))) continue;
        if (json) puts("{\"operation\":\"download\",\"status\":\"failed\",\"error\":\"invalid-utf8\"}");
        else fputs("brw download: invalid-utf8\n", stderr);
        return 1;
    }
    const char *error = digby_download(&plan, preview);
    const char *status = error ? "failed" : preview ? "planned" : "applied";
    if (json) {
        printf("{\"operation\":\"download\",\"status\":\"%s\",\"path\":", status);
        quoted(plan.destination);
        fputs(",\"sha256\":", stdout);
        quoted(plan.sha256);
        if (!error) {
            fputs(",\"url\":", stdout);
            quoted(plan.url);
        }
        if (preview && !error) fputs(",\"remoteStatus\":\"unverified\"", stdout);
        if (error) { fputs(",\"error\":", stdout); quoted(error); }
        puts("}");
    } else if (error) {
        brw_download_diagnostic(plan.destination, error);
    } else {
        printf("%s download %s -> %s (SHA-256 %s)\n", status, plan.url, plan.destination, plan.sha256);
        if (preview) puts("Remote availability and SHA-256 verification are pending.");
    }
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_DOWNLOAD_USAGE "\n", stderr);
    return 2;
}
