#ifndef BRW_DIGBY_H
#define BRW_DIGBY_H

#include <stddef.h>

enum digby_operation {
    DIGBY_VERIFY, DIGBY_EXTRACT, DIGBY_PUBLISH, DIGBY_ACTIVATE, DIGBY_REMOVE, DIGBY_DOWNLOAD
};

enum digby_action { DIGBY_INSTALL, DIGBY_UPDATE, DIGBY_ROLLBACK, DIGBY_UNINSTALL };

struct digby_plan {
    enum digby_operation steps[4];
    size_t count;
};

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
