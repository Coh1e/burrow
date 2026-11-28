#ifndef BRW_DIGBY_DOWNLOAD_H
#define BRW_DIGBY_DOWNLOAD_H

#include <stdbool.h>

/* All strings are borrowed, NUL-terminated UTF-8, valid for the call. */
struct digby_download_plan {
    const char *workspace, *url, *destination, *sha256;
};

/* Check the host transport without opening a connection. */
bool digby_download_available(void);
/* Validate operands without filesystem or network access. Returns a static
   diagnostic or NULL. Blueprint uses this when the parent is produced later. */
const char *digby_download_validate(const struct digby_download_plan *plan);
/* Preview also checks the local destination. Apply publishes only a complete
   verified file at a new path. Returns NULL on success or a static diagnostic. */
const char *digby_download(const struct digby_download_plan *plan, bool preview);

#endif
