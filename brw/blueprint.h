#ifndef BRW_BLUEPRINT_H
#define BRW_BLUEPRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum bp_op { BP_COPY, BP_REPLACE, BP_INSTALL, BP_ENV, BP_RUN, BP_STATE, BP_TOOL, BP_OP_COUNT };
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
    unsigned required, unavailable; /* 1u << enum bp_op */
    char platform[8]; /* Host platform supplied to bp_load. */
};

struct bp_output {
    char *name, *source; /* Owned declaration name and include position. */
    size_t end; /* Successful step prefix at which the call returns. */
};

struct bp_binding {
    size_t output;
    bool writer;
};

struct bp_step {
    struct bp_state *state; /* Owned by a state step. */
    struct bp_binding *bindings; /* Parallel to args. Output indices are one-based. */
    char *skip_platform, *skip_op, *registry;
    enum bp_op op;
    char *source; /* Call path including the position within this module. */
    char *path, *from, *to, *name, *version, *entry, *value;
    char *program, **args; /* Owned strings. The staged payload occupies script_arg. */
    size_t argc, script_arg;
    size_t count;
    const struct bp_file *payload; /* Borrowed from the verified bundle. */
};

enum bp_state_status { BP_UNCHECKED, BP_SATISFIED, BP_UNMET, BP_CHANGED, BP_DEFERRED };

struct bp_state {
    struct bp_step check, change;
    unsigned char unmet[256]; /* Portable process exit codes 1..255. */
    enum bp_state_status status;
    unsigned long exit_code;
    bool has_exit_code;
};

struct bp_plan {
    struct bp_lock *lock; /* Borrowed for the duration of CLI execution. */
    struct bp_step steps[BP_MAX_STEPS];
    struct bp_output outputs[BP_MAX_STEPS];
    size_t output_count;
    size_t checks[BP_MAX_STEPS], check_count; /* Output indices in call-return order. */
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
   completed counts successful operations, excluding skipped steps. On error
   failure_source borrows the failing step or producer position from plan. */
const char *digby_run_blueprint(const struct bp_plan *plan, const char *workspace, bool preview,
                                size_t *completed, const char **failure_source);

/* Create takes a zero-initialized file. Success owns a UTF-8 path and read
   stream until remove closes the stream, deletes the file and frees the path. */
struct bp_output_file {
    char *path;
    FILE *stream;
};
bool bp_create_output_file(struct bp_output_file *file);
bool bp_remove_output_file(struct bp_output_file *file);

/* Validates operands against an existing workspace without applying the step. */
const char *bp_validate_step(const char *workspace, const struct bp_step *step);
/* Checks replacement inputs/outputs, including values produced by earlier
   steps. failed_step receives the zero-based failing step on error. */
const char *bp_validate_text_plan(const char *workspace, const struct bp_plan *plan,
                                  size_t *failed_step);
const char *bp_apply_step(const char *workspace, const struct bp_step *step);
/* With apply false, validate without launching or writing exit_code. With apply
   true, execute the command. If exit_code is supplied, normal exits (including
   nonzero) store the child status. Launch, wait and staging failures return errors. */
const char *bp_run_command(const char *workspace, const struct bp_step *s, bool apply,
                           unsigned long *exit_code);
const char *bp_op_name(enum bp_op op);

#endif
