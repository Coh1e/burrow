#include "digby.h"
#include "blueprint.h"

const char *bp_op_name(enum bp_op op)
{
    static const char *const names[BP_OP_COUNT] = {"copy", "replace", "install", "env", "run", "state", "tool", "default", "host"};
    return (unsigned)op < BP_OP_COUNT ? names[op] : "unknown";
}

const char *digby_plan_setting(const struct digby_setting *s, unsigned capabilities,
                               struct digby_setting_plan *out)
{
    if (!out) return "invalid-plan";
    out->ready = false;
    if (!s || !s->name || !*s->name || (s->expect_missing && s->expected))
        return "invalid-setting";
    unsigned required;
    bool needs_value;
    if (s->scope == SETTING_REGISTRY) {
        if (!s->key || !*s->key || (s->type != SETTING_STRING && s->type != SETTING_DWORD))
            return "invalid-setting";
        required = DIGBY_CAP_REGISTRY;
        switch (s->op) {
        case DIGBY_REGISTRY_WRITE: needs_value = true; break;
        case DIGBY_REGISTRY_READ:
        case DIGBY_REGISTRY_DELETE: needs_value = false; break;
        default: return "invalid-action";
        }
        if ((s->op != DIGBY_REGISTRY_READ) != (s->expected != NULL || s->expect_missing))
            return "invalid-precondition";
    } else {
        if ((s->scope != SETTING_USER && s->scope != SETTING_SESSION) || s->key || s->type != SETTING_STRING)
            return "invalid-scope";
        required = s->scope == SETTING_USER ? DIGBY_CAP_USER_ENV : DIGBY_CAP_SESSION_ENV;
        switch (s->op) {
        case DIGBY_ENV_SET:
        case DIGBY_PATH_ADD:
        case DIGBY_PATH_REMOVE: needs_value = true; break;
        case DIGBY_ENV_REMOVE: needs_value = false; break;
        default: return "invalid-action";
        }
    }
    if (!(capabilities & required)) return "missing-capability";
    if (needs_value != (s->argument != NULL)) return "invalid-value";
    out->step = *s;
    out->ready = true;
    return NULL;
}

const char *digby_execute_setting(const struct digby_setting_plan *plan, digby_setting_step backend,
                                  void *context)
{
    if (!plan || !plan->ready || !backend) return "invalid-plan";
    return backend(context, &plan->step);
}

struct digby_plan digby_install_plan(void)
{
    const struct digby_plan plan = {{DIGBY_VERIFY, DIGBY_EXTRACT, DIGBY_PUBLISH}, 3};
    return plan;
}

struct digby_plan digby_source_install_plan(void)
{
    const struct digby_plan plan = {{DIGBY_DOWNLOAD, DIGBY_VERIFY, DIGBY_EXTRACT, DIGBY_PUBLISH}, 4};
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
    case DIGBY_GIT_CLONE: return "git-clone";
    case DIGBY_GIT_PULL: return "git-pull";
    case DIGBY_SSH: return "ssh";
    case DIGBY_DOWNLOAD: return "download";
    case DIGBY_VERIFY: return "verify";
    case DIGBY_EXTRACT: return "extract";
    case DIGBY_PUBLISH: return "publish";
    case DIGBY_ACTIVATE: return "activate";
    case DIGBY_REMOVE: return "remove";
    case DIGBY_ENV_SET: return "env-set";
    case DIGBY_ENV_REMOVE: return "env-remove";
    case DIGBY_PATH_ADD: return "path-add";
    case DIGBY_PATH_REMOVE: return "path-remove";
    case DIGBY_REGISTRY_READ: return "registry-read";
    case DIGBY_REGISTRY_WRITE: return "registry-write";
    case DIGBY_REGISTRY_DELETE: return "registry-delete";
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
