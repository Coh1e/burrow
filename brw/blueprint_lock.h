#ifndef BRW_BLUEPRINT_LOCK_H
#define BRW_BLUEPRINT_LOCK_H

#include "blueprint.h"

/* Rows own a single allocation containing their immutable catalog fields. */
struct bp_package {
    char *storage;
    const char *registry, *name, *version, *kind, *location, *digest, *entry, *depends;
    size_t first_step;
    bool reused;
};

struct bp_lock {
    struct bp_package *rows;
    size_t count;
};

/* Caller owns a zero-initialized lock and frees it even on failure. Selected
   rows are in dependency order. Generation writes only the host lock file. */
const char *bp_prepare_lock(struct bp_plan *plan, const struct bp_bundle *bundle,
                            const char *directory, const char *target, bool generate,
                            struct bp_lock *lock, char detail[BP_DETAIL_CAP]);
void bp_lock_free(struct bp_lock *lock);
const char *bp_apply_packages(struct bp_lock *lock, size_t step);
bool bp_package_id(const char *text);
/* Reads the remainder of the borrowed stream, starting at its current position. */
bool bp_matches_digest(FILE *file, const char *expected);
/* Caller owns the absolute path derived from the locked version and entry. */
char *bp_version_entry(const struct bp_package *p);
/* Materialize the selected package and its locked dependencies. On success, the
   caller owns the executable and an optional PATH prefix for this child only. */
const char *bp_use_tool(const struct bp_lock *lock, const struct bp_step *step,
                        char **program, char **child_path);
const char *bp_use_default(const struct bp_lock *lock, const struct bp_step *step, bool *changed);

/* Platform adapters. All strings are UTF-8. Visitor arguments are borrowed.
   Path results are caller-owned. A failed atomic write preserves the old file. */
typedef const char *(*bp_catalog_visit)(const char *source, const char *path, void *context);
const char *bp_catalogs(bp_catalog_visit visit, void *context);
const char *bp_validate_catalog(const char *source, const char *path);
char *bp_cache_dir(bool create);
char *bp_data_dir(void);
FILE *bp_open_read(const char *path);
typedef const char *(*bp_write_file)(FILE *file, void *context);
const char *bp_atomic_write(const char *path, bp_write_file write, void *context);
const char *bp_fetch_artifact(const char *location, FILE *file);
char *bp_join(const char *dir, const char *name);
extern const char bp_path_separator;
/* Join a package identity to its parent using this platform's on-disk spelling. */
char *bp_package_child(const char *parent, const char *name);
/* Materialize the verified version without publishing a user command entry.
   On success the caller owns *program. Diagnostic codes are static. */
const char *bp_materialize_tool(const struct bp_package *p, const char *cache, char **program);
/* Return the owned absolute entry path without writing. Overrides are earlier
   env declarations; NULL selects the corresponding process environment value. */
extern const char bp_home_key[];
bool bp_env_key_equal(const char *name, const char *key);
char *bp_default_entry(const struct bp_package *p, const char *bin_override, const char *home_override);
/* Inspect or switch the managed entry, preserving unrelated user commands. */
const char *bp_default_matches(const struct bp_package *p, bool *matches);
const char *bp_set_default(const struct bp_package *p, const char *cache, bool *changed);
#endif
