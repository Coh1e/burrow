#ifndef BRW_DIGBY_ARCHIVE_IO_H
#define BRW_DIGBY_ARCHIVE_IO_H

#include "blueprint_bundle_io.h"
#include <stdbool.h>
#include <stdio.h>

struct digby_archive_stage;
struct digby_file_stage;

/* The check and both begin functions borrow the opened workspace and validated
   relative path for the call. A successful begin pins its own output parent. */
const char *digby_check_destination(const struct bp_directory *workspace, const char *relative);
const char *digby_archive_begin(const struct bp_directory *workspace, const char *relative,
                                struct digby_archive_stage **out);

/* Add one validated member, creating parents as needed. For a regular file,
   success returns an exclusive binary stream; the caller must close it. */
const char *digby_archive_entry(struct digby_archive_stage *stage, const char *relative,
                                bool directory, bool executable, FILE **out);

/* Consumes the stage, including on failure. Publication never overwrites an
   existing destination. Rollback removes owned entries. A stage-changed or
   cleanup-stage error can leave the staging directory for inspection. */
const char *digby_archive_finish(struct digby_archive_stage *stage, bool publish);

/* Stages one file beside its validated workspace-relative target for pack or download.
   Success returns an owned stage and its borrowed binary output stream.
   Finish closes the stream and consumes the stage on every path. Publication
   never replaces a concurrent output. On POSIX the file stays inside a private
   directory until publication. Detected stage changes return stage-changed.
   If directory cleanup fails after publication, the complete output remains.
   On publish, an optional before_publish callback borrows context after close
   and ownership checks, just before rename. A static error vetoes publication. */
const char *digby_file_begin(const struct bp_directory *workspace, const char *relative,
                             struct digby_file_stage **out, FILE **stream);
const char *digby_file_finish(struct digby_file_stage *stage, bool publish,
                              const char *(*before_publish)(void *context), void *context);

#endif
