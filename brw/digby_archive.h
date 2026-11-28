#ifndef BRW_DIGBY_ARCHIVE_H
#define BRW_DIGBY_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>

enum { DIGBY_ARCHIVE_MEMBER_LIMIT = 4096, DIGBY_ARCHIVE_EXPANDED_LIMIT = 256 * 1024 * 1024 };

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

/* All strings and the ordered inputs array are borrowed for the call.
   Each input names an ordinary workspace file and its path within the archive.
   destination is a new workspace file. format is tar, tar.gz or zip. */
struct digby_pack_plan {
    const char *workspace, *destination, *format;
    const char *const *inputs;
    size_t count;
};

/* Validate path relationships, format and input count without filesystem access.
   All pack functions return NULL on success or a static error code. */
const char *digby_pack_validate_operands(const struct digby_pack_plan *plan);

/* Read-only preflight for a Blueprint with inputs produced by earlier steps.
   An optional mask of count booleans skips only those future inputs. A future
   destination parent can be deferred separately. Operands are always checked. */
const char *digby_pack_preflight(const struct digby_pack_plan *plan,
                                 const bool *defer_inputs, bool defer_destination);

/* Preview reads every input without staging files. Apply stages the archive and
   publishes it without replacing an existing file, after the writer closes. */
const char *digby_archive_pack(const struct digby_pack_plan *plan, bool preview);

#endif
