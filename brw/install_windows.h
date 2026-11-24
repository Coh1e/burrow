#ifndef BRW_INSTALL_WINDOWS_H
#define BRW_INSTALL_WINDOWS_H

#include "digby.h"
#include <stdbool.h>
#include <wchar.h>

struct local_package {
    const wchar_t *name;
    const wchar_t *version;
    const wchar_t *archive;
    const wchar_t *digest;
    const wchar_t *entry;
};

/* Input strings are borrowed for the duration of the call. Errors are static. */
const char *brw_validate_package(const struct local_package *package);
const char *brw_validate_lifecycle(enum digby_action action, const struct local_package *package);
const char *brw_install_package(const struct local_package *package,
                                const struct digby_plan *plan, size_t *failed_step);
const char *brw_run_lifecycle(enum digby_action action, const struct local_package *package,
                             const struct digby_plan *plan, size_t *failed_step);

bool brw_package_component(const wchar_t *name);
const char *brw_validate_source_package(const struct local_package *package, bool executable);
/* Borrows the validated package and plan for the call. Owns and closes all stage
   handles. Returns NULL on success or a static error code. If supplied, sets
   failed_step to the zero-based failed operation, or zero before execution. */
const char *brw_install_source_package(const struct local_package *package, bool executable,
                                       const struct digby_plan *plan, size_t *failed_step);
int brw_list_installed(bool json);
int brw_report_plan(bool json, bool preview, const char *status,
                    const struct digby_plan *plan, const char *error, size_t step);

#endif
