#include "blueprint.h"

const char *digby_run_blueprint(const struct bp_plan *plan, const char *workspace, bool preview,
                                size_t *completed, size_t *failed_step)
{
    *completed = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        const char *error = bp_validate_step(workspace, &plan->steps[i]);
        if (error) {
            *failed_step = i;
            return error;
        }
    }
    const char *error = bp_validate_text_plan(workspace, plan, failed_step);
    if (error || preview) return error;
    for (size_t i = 0; i < plan->count; ++i) {
        error = bp_apply_step(workspace, &plan->steps[i]);
        if (error) {
            *failed_step = i;
            return error;
        }
        ++*completed;
    }
    return NULL;
}
