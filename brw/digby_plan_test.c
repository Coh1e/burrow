#include "digby.h"
#include <string.h>

struct observation {
    enum digby_operation seen[4];
    size_t count;
    int fail_extraction;
};

static const char *observe(void *context, enum digby_operation operation)
{
    struct observation *result = context;
    result->seen[result->count++] = operation;
    return result->fail_extraction && operation == DIGBY_EXTRACT ? "extract-failed" : NULL;
}

static const char *observe_setting(void *context, const struct digby_setting *step)
{
    *(struct digby_setting *)context = *step;
    return NULL;
}

int main(void)
{
    struct digby_plan plan = digby_install_plan();
    struct observation result = {0};
    size_t failed;
    if (digby_execute(&plan, observe, &result, &failed) || result.count != 3 ||
        result.seen[0] != DIGBY_VERIFY || result.seen[1] != DIGBY_EXTRACT ||
        result.seen[2] != DIGBY_PUBLISH)
        return 1;
    result.count = 0;
    result.fail_extraction = 1;
    const char *error = digby_execute(&plan, observe, &result, &failed);
    if (!error || strcmp(error, "extract-failed") || failed != 1 || result.count != 2)
        return 2;
    plan.count = sizeof plan.steps / sizeof *plan.steps + 1;
    result.count = 0;
    if (!digby_execute(&plan, observe, &result, NULL) || result.count)
        return 3;
    plan = digby_source_install_plan();
    error = digby_execute(&plan, observe, &result, &failed);
    if (!error || strcmp(error, "extract-failed") || failed != 2 || result.count != 3 ||
        result.seen[0] != DIGBY_DOWNLOAD || result.seen[1] != DIGBY_VERIFY)
        return 4;
    result.count = 0;
    result.fail_extraction = 0;
    if (digby_execute(&plan, observe, &result, NULL) || result.count != 4 ||
        result.seen[3] != DIGBY_PUBLISH)
        return 5;
    plan = digby_lifecycle_plan(DIGBY_ROLLBACK);
    result.count = 0;
    if (digby_execute(&plan, observe, &result, NULL) ||
        result.count != 1 || result.seen[0] != DIGBY_ACTIVATE)
        return 6;
    plan = digby_lifecycle_plan(DIGBY_UNINSTALL);
    result.count = 0;
    if (digby_execute(&plan, observe, &result, NULL) ||
        result.count != 1 || result.seen[0] != DIGBY_REMOVE) return 7;

    struct digby_setting_plan settings_plan;
    struct digby_setting setting = {SETTING_REGISTRY, DIGBY_REGISTRY_WRITE, SETTING_STRING,
                                    L"key", L"name", L"new", NULL, true};
    error = digby_plan_setting(&setting, DIGBY_CAP_SESSION_ENV, &settings_plan);
    if (!error || strcmp(error, "missing-capability") || settings_plan.ready) return 8;
    if (digby_plan_setting(&setting, DIGBY_CAP_REGISTRY, &settings_plan) ||
        !settings_plan.ready || settings_plan.step.op != DIGBY_REGISTRY_WRITE) return 9;
    setting.expect_missing = false;
    error = digby_plan_setting(&setting, DIGBY_CAP_REGISTRY, &settings_plan);
    if (!error || strcmp(error, "invalid-precondition") || settings_plan.ready) return 10;
    setting.scope = SETTING_SESSION;
    setting.key = NULL;
    setting.op = DIGBY_ENV_SET;
    if (digby_plan_setting(&setting, DIGBY_CAP_SESSION_ENV, &settings_plan) ||
        !settings_plan.ready || settings_plan.step.op != DIGBY_ENV_SET) return 11;
    setting.scope = SETTING_USER;
    struct digby_setting seen;
    if (digby_execute_setting(&settings_plan, observe_setting, &seen) ||
        seen.scope != SETTING_SESSION || seen.op != DIGBY_ENV_SET || wcscmp(seen.argument, L"new"))
        return 13;
    error = digby_plan_setting(&setting, DIGBY_CAP_SESSION_ENV, &settings_plan);
    return !error || strcmp(error, "missing-capability") || settings_plan.ready ? 12 : 0;
}
