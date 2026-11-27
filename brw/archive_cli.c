#include "archive_cli.h"
#include "blueprint_bundle_io.h"
#include "digby_archive.h"
#include "json_utf8.h"
#include "verified_file.h"
#include <stdlib.h>
#include <string.h>

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
    } else if (error) fprintf(stderr, "brw unpack: %s\n", error);
    else printf("%s unpack %s (SHA-256 %s) -> %s\n", status, argv[0], digest, argv[1]);
    return error ? 1 : 0;
usage:
    fputs("usage: " BRW_UNPACK_USAGE "\n", stderr);
    return 2;
}
