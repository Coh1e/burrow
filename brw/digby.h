#ifndef BRW_DIGBY_H
#define BRW_DIGBY_H

#include <stddef.h>
#include <stdbool.h>
#include <wchar.h>

enum digby_operation {
    DIGBY_VERIFY, DIGBY_EXTRACT, DIGBY_PUBLISH, DIGBY_ACTIVATE, DIGBY_REMOVE, DIGBY_DOWNLOAD,
    DIGBY_ENV_SET, DIGBY_ENV_REMOVE, DIGBY_PATH_ADD, DIGBY_PATH_REMOVE,
    DIGBY_REGISTRY_READ, DIGBY_REGISTRY_WRITE, DIGBY_REGISTRY_DELETE,
    DIGBY_GIT_CLONE, DIGBY_GIT_PULL, DIGBY_SSH
};

enum digby_action { DIGBY_INSTALL, DIGBY_UPDATE, DIGBY_ROLLBACK, DIGBY_UNINSTALL };

struct digby_plan {
    enum digby_operation steps[4];
    size_t count;
};

/* Settings inputs are borrowed NUL-terminated strings, valid through execution.
   A NULL expected value leaves the precondition unspecified unless expect_missing
   is set. Registry mutations require one of these two preconditions. */
enum digby_setting_scope { SETTING_REGISTRY, SETTING_USER, SETTING_SESSION };
enum digby_setting_type { SETTING_STRING, SETTING_DWORD };
enum digby_setting_capability {
    DIGBY_CAP_REGISTRY = 1, DIGBY_CAP_USER_ENV = 2, DIGBY_CAP_SESSION_ENV = 4
};
struct digby_setting {
    enum digby_setting_scope scope;
    enum digby_operation op;
    enum digby_setting_type type;
    const wchar_t *key, *name, *argument, *expected;
    bool expect_missing;
};
struct digby_setting_plan {
    struct digby_setting step;
    bool ready;
};
/* Copies the authorized step. Its borrowed string contents must stay unchanged
   through execution. Failure clears ready and returns a static diagnostic. */
const char *digby_plan_setting(const struct digby_setting *setting, unsigned capabilities,
                               struct digby_setting_plan *out);
typedef const char *(*digby_setting_step)(void *context, const struct digby_setting *step);
const char *digby_execute_setting(const struct digby_setting_plan *plan, digby_setting_step backend,
                                  void *context);

/* The backend owns its context and any resources opened by steps. */
typedef const char *(*digby_step)(void *context, enum digby_operation operation);

struct digby_plan digby_install_plan(void);
struct digby_plan digby_source_install_plan(void);
struct digby_plan digby_lifecycle_plan(enum digby_action action);
const char *digby_operation_name(enum digby_operation operation);
/* Returns a static error code or NULL. When supplied, failed_step is zero-based. */
const char *digby_execute(const struct digby_plan *plan, digby_step step,
                          void *context, size_t *failed_step);

#endif
