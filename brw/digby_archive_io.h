#ifndef BRW_DIGBY_ARCHIVE_IO_H
#define BRW_DIGBY_ARCHIVE_IO_H

#include <stdbool.h>
#include <stdio.h>

struct digby_archive_stage;

/* Read-only check. The caller validated the workspace-relative destination.
   begin rechecks it and returns an owned stage on success. */
const char *digby_archive_destination(const char *workspace, const char *relative);
const char *digby_archive_begin(const char *workspace, const char *relative,
                                struct digby_archive_stage **out);

/* Add one validated member, creating parents as needed. For a regular file,
   success returns an exclusive binary stream; the caller must close it. */
const char *digby_archive_entry(struct digby_archive_stage *stage, const char *relative,
                                bool directory, bool executable, FILE **out);

/* Consumes the stage, including on failure. Publication never overwrites an
   existing destination. Rollback removes owned entries. A stage-changed or
   cleanup-stage error can leave the staging directory for inspection. */
const char *digby_archive_finish(struct digby_archive_stage *stage, bool publish);

#endif
