#ifndef BRW_BLUEPRINT_BUNDLE_IO_H
#define BRW_BLUEPRINT_BUNDLE_IO_H

#include <stddef.h>

struct bp_directory;

/* Keeps the bundle root open until close. Paths passed to read are validated
   package-relative paths. A successful read returns an owned byte snapshot
   with a trailing NUL outside its size. Neither routine follows package links. */
struct bp_directory *bp_directory_open(const char *path);
void bp_directory_close(struct bp_directory *dir);
/* Read an operator-supplied local file into an owned, size-limited snapshot.
   On failure out and size are left untouched. */
const char *brw_read_local_file(const char *path, size_t limit, unsigned char **out, size_t *size);
const char *bp_directory_read(const struct bp_directory *dir, const char *path,
                              size_t limit, unsigned char **out, size_t *size);

#endif
