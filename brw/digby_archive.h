#ifndef BRW_DIGBY_ARCHIVE_H
#define BRW_DIGBY_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>

/* All inputs are borrowed for the call. data is the exact byte snapshot
   whose digest the caller verified. destination names a new directory
   relative to workspace. Its parent must exist at extraction time. */
struct digby_archive_plan {
    const unsigned char *data;
    size_t size;
    const char *workspace;
    const char *destination;
};

/* Checks operands and every member using only the snapshot. A planner can
   defer checking the destination when a preceding step creates its parent.
   Returns NULL on success or a static error code. */
const char *digby_archive_validate(const struct digby_archive_plan *plan);

/* Returns NULL on success or a static error code. Preview reads and checks
   the archive without staging files. Apply publishes the whole tree once. */
const char *digby_archive_extract(const struct digby_archive_plan *plan, bool preview);

#endif
