#ifndef BRW_BLUEPRINT_H
#define BRW_BLUEPRINT_H

#include <stdbool.h>
#include <stddef.h>

enum bp_op { BP_COPY, BP_REPLACE, BP_INSTALL, BP_ENV, BP_RUN, BP_OP_COUNT };
enum { BP_MAX_ARGS = 128, BP_MAX_FILES = 128, BP_MAX_STEPS = 256, BP_DETAIL_CAP = 512 };

struct bp_file {
    char *path;
    char digest[65];
    unsigned char *data; /* Owned, with a trailing NUL outside size. */
    size_t size;
    bool module;
};

struct bp_bundle {
    struct bp_file files[BP_MAX_FILES];
    size_t count;
    char *entry;
    unsigned required; /* 1u << enum bp_op */
};

struct bp_step {
    enum bp_op op;
    char *source; /* Call path including the position within this module. */
    char *path, *from, *to, *name, *version, *entry, *value;
    char *program, **args; /* Owned strings. The staged payload occupies script_arg. */
    size_t argc, script_arg;
    size_t count;
    const struct bp_file *payload; /* Borrowed from the verified bundle. */
};

struct bp_plan {
    struct bp_step steps[BP_MAX_STEPS];
    size_t count;
};

/* Inputs are borrowed UTF-8. Caller supplies zero-initialized bundle and plan,
   and calls the corresponding free function even after failure. Diagnostics
   returned are static codes. Detail, if populated, is NUL-terminated. */
const char *bp_load(const char *directory, const char *digest, const char *platform,
                    const char *arch, unsigned capabilities, struct bp_bundle *out,
                    char detail[BP_DETAIL_CAP]);
void bp_bundle_free(struct bp_bundle *bundle);
bool bp_safe_path(const char *path);
const struct bp_file *bp_find(const struct bp_bundle *bundle, const char *path);
const char *bp_build(const struct bp_bundle *bundle, struct bp_plan *out, char detail[BP_DETAIL_CAP]);
void bp_plan_free(struct bp_plan *plan);

/* Runs a successfully built plan against the selected platform backend.
   completed receives its executed prefix length; on error failed_step
   identifies the zero-based step whose validation or execution failed. */
const char *digby_run_blueprint(const struct bp_plan *plan, const char *workspace, bool preview,
                                size_t *completed, size_t *failed_step);

/* The backend validates every step before the caller applies the first one.
   The workspace must already exist. Only bp_apply_step may change state. */
const char *bp_validate_step(const char *workspace, const struct bp_step *step);
/* Checks replacement inputs/outputs, including values produced by earlier
   steps. failed_step receives the zero-based failing step on error. */
const char *bp_validate_text_plan(const char *workspace, const struct bp_plan *plan,
                                  size_t *failed_step);
const char *bp_apply_step(const char *workspace, const struct bp_step *step);
const char *bp_op_name(enum bp_op op);

#endif
