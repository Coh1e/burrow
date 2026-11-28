#include "digby_download.h"
#include "digby_download_io.h"
#include "digby_archive_io.h"
#include "verified_file.h"
#include "sha.h"
#include <string.h>

const char *digby_download_validate(const struct digby_download_plan *plan)
{
    if (!plan || !plan->workspace || !*plan->workspace ||
        !bp_valid_utf8(plan->workspace, strlen(plan->workspace))) return "invalid-workspace";
    if (!bp_safe_path(plan->destination)) return "unsafe-destination";
    if (!brw_valid_sha256(plan->sha256)) return "invalid-digest";
    return brw_valid_http_url(plan->url) ? NULL : "download-url";
}

struct download_sink {
    FILE *stream;
    SHA256Context digest;
    size_t size;
};

static const char *write_chunk(void *context, const unsigned char *bytes, size_t length)
{
    struct download_sink *sink = context;
    if (length > BRW_PAYLOAD_LIMIT - sink->size) return "download-limit";
    if (fwrite(bytes, 1, length, sink->stream) != length) return "download-stage";
    if (SHA256Input(&sink->digest, bytes, (unsigned)length)) return "digest-failed";
    sink->size += length;
    return NULL;
}

static const char *check_download_cancelled(void *context)
{
    return digby_download_cancelled(context) ? "cancelled" : NULL;
}

const char *digby_download(const struct digby_download_plan *plan, bool preview)
{
    const char *error = digby_download_validate(plan);
    if (error) return error;
    if (!digby_download_available()) return "missing-capability";
    struct bp_directory *workspace = bp_directory_open(plan->workspace);
    if (!workspace) return "unsafe-parent";
    if (preview) {
        error = digby_check_destination(workspace, plan->destination);
        bp_directory_close(workspace);
        return error;
    }

    struct digby_download_control *control = NULL;
    struct digby_file_stage *stage = NULL;
    struct download_sink sink = {0};
    error = digby_download_control_begin(&control);
    if (!error) error = digby_file_begin(workspace, plan->destination, &stage, &sink.stream);
    bp_directory_close(workspace);
    if (!error && SHA256Reset(&sink.digest)) error = "digest-failed";
    if (!error) error = digby_download_transfer(control, plan->url, write_chunk, &sink);
    unsigned char hash[SHA256HashSize];
    if (!error && SHA256Result(&sink.digest, hash)) error = "digest-failed";
    if (!error && !brw_sha256_digest_matches(hash, plan->sha256)) error = "file-digest-mismatch";
    /* A cancellation observed before publication rolls back only our stage. */
    if (control && digby_download_cancelled(control)) error = "cancelled";
    if (stage) {
        const char *finish = digby_file_finish(stage, !error, check_download_cancelled, control);
        if (finish) error = finish;
    }
    if (control) digby_download_control_end(control);
    return error;
}
