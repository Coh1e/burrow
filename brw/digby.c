#include "digby.h"

struct digby_plan digby_install_plan(void)
{
    const struct digby_plan plan = {{DIGBY_VERIFY, DIGBY_EXTRACT, DIGBY_PUBLISH}, 3};
    return plan;
}

struct digby_plan digby_lifecycle_plan(enum digby_action action)
{
    switch (action) {
    case DIGBY_INSTALL:
    case DIGBY_UPDATE:
        return digby_install_plan();
    case DIGBY_ROLLBACK: {
        const struct digby_plan plan = {{DIGBY_ACTIVATE}, 1};
        return plan;
    }
    case DIGBY_UNINSTALL: {
        const struct digby_plan plan = {{DIGBY_REMOVE}, 1};
        return plan;
    }
    }
    const struct digby_plan invalid = {{0}, 0};
    return invalid;
}

const char *digby_operation_name(enum digby_operation operation)
{
    switch (operation) {
    case DIGBY_VERIFY: return "verify";
    case DIGBY_EXTRACT: return "extract";
    case DIGBY_PUBLISH: return "publish";
    case DIGBY_ACTIVATE: return "activate";
    case DIGBY_REMOVE: return "remove";
    }
    return "unknown";
}

const char *digby_execute(const struct digby_plan *plan, digby_step step,
                          void *context, size_t *failed_step)
{
    if (failed_step)
        *failed_step = 0;
    if (!plan || !step || !plan->count || plan->count > sizeof plan->steps / sizeof *plan->steps)
        return "invalid-plan";
    for (size_t i = 0; i < plan->count; ++i) {
        const char *error = step(context, plan->steps[i]);
        if (error) {
            if (failed_step)
                *failed_step = i;
            return error;
        }
    }
    return NULL;
}
