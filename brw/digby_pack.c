#include "digby_archive.h"
#include "digby_archive_io.h"
#include "blueprint_bundle_io.h"
#include "verified_file.h"
#include <archive.h>
#include <archive_entry.h>
#include <stdlib.h>
#include <string.h>

struct pack_output {
    FILE *stream; /* NULL counts serialized bytes for read-only preflight. */
    size_t size;
    const char *error;
};

static la_ssize_t write_block(struct archive *writer, void *context,
                              const void *bytes, size_t size)
{
    (void)writer;
    struct pack_output *out = context;
    if (size > BRW_PAYLOAD_LIMIT - out->size) out->error = "archive-limit";
    else if (out->stream && fwrite(bytes, 1, size, out->stream) != size) out->error = "archive-output";
    if (out->error) return -1;
    out->size += size;
    return (la_ssize_t)size;
}

static const char *open_writer(struct archive *writer, const char *format, struct pack_output *out)
{
    int status = !strcmp(format, "zip") ? archive_write_set_format_zip(writer) :
                                         archive_write_set_format_pax_restricted(writer);
    if (status != ARCHIVE_OK) return "archive-format";
    if (!strcmp(format, "tar.gz") &&
        (archive_write_add_filter_gzip(writer) != ARCHIVE_OK ||
         archive_write_set_filter_option(writer, "gzip", "timestamp", NULL) != ARCHIVE_OK))
        return "archive-format";
    /* Use the exact serialized size, including headers and any format trailer. */
    if (archive_write_set_bytes_in_last_block(writer, 1) != ARCHIVE_OK ||
        archive_write_open2(writer, out, NULL, write_block, NULL, NULL) != ARCHIVE_OK)
        return "archive-output";
    return NULL;
}

static const char *write_input(struct archive *writer, const struct bp_directory *workspace,
                               const char *path, size_t *remaining)
{
    unsigned char *bytes;
    size_t size;
    size_t limit = *remaining < BRW_PAYLOAD_LIMIT ? *remaining : BRW_PAYLOAD_LIMIT;
    const char *error = bp_directory_read(workspace, path, limit, &bytes, &size);
    if (error) {
        if (!strcmp(error, "bundle-limit")) return "archive-limit";
        if (!strcmp(error, "bundle-file-unavailable")) return "archive-input-unavailable";
        return error;
    }
    struct archive_entry *entry = archive_entry_new();
    if (!entry) error = "out-of-memory";
    else {
        archive_entry_set_pathname(entry, path);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, (la_int64_t)size);
        /* ZIP timestamps start in 1980. Use a fixed representable instant on all hosts. */
        archive_entry_set_mtime(entry, 315532800, 0);
        if (archive_write_header(writer, entry) != ARCHIVE_OK ||
            archive_write_data(writer, bytes, size) != (la_ssize_t)size ||
            archive_write_finish_entry(writer) != ARCHIVE_OK) error = "archive-output";
        archive_entry_free(entry);
    }
    free(bytes);
    *remaining -= size;
    return error;
}

static const char *write_archive(const struct digby_pack_plan *plan, const bool *defer_inputs,
                                 const struct bp_directory *workspace, FILE *stream)
{
    struct archive *writer = archive_write_new();
    if (!writer) return "out-of-memory";
    struct pack_output out = {.stream = stream};
    const char *error = open_writer(writer, plan->format, &out);
    size_t remaining = DIGBY_ARCHIVE_EXPANDED_LIMIT;
    for (size_t i = 0; !error && i < plan->count; ++i) {
        if (!defer_inputs || !defer_inputs[i])
            error = write_input(writer, workspace, plan->inputs[i], &remaining);
    }
    if (archive_write_close(writer) != ARCHIVE_OK && !error) error = "archive-output";
    if (archive_write_free(writer) != ARCHIVE_OK && !error) error = "archive-output";
    return out.error ? out.error : error;
}

static const char *pack(const struct digby_pack_plan *plan, bool preview,
                        const bool *defer_inputs, bool defer_destination)
{
    const char *error = digby_pack_validate_operands(plan);
    if (error) return error;
    struct bp_directory *workspace = bp_directory_open(plan->workspace);
    if (!workspace) return "unsafe-parent";
    struct digby_pack_stage *stage = NULL;
    FILE *stream = NULL;
    if (!preview) error = digby_pack_begin(plan->workspace, plan->destination, &stage, &stream);
    else if (!defer_destination) error = digby_archive_destination(plan->workspace, plan->destination);
    if (!error) error = write_archive(plan, defer_inputs, workspace, stream);
    bp_directory_close(workspace);
    if (stage) {
        const char *finish = digby_pack_finish(stage, !error);
        if (finish) return finish;
    }
    return error;
}

const char *digby_pack_preflight(const struct digby_pack_plan *plan,
                                 const bool *defer_inputs, bool defer_destination)
{
    return pack(plan, true, defer_inputs, defer_destination);
}

const char *digby_archive_pack(const struct digby_pack_plan *plan, bool preview)
{
    return pack(plan, preview, NULL, false);
}
