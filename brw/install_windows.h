#ifndef BRW_INSTALL_WINDOWS_H
#define BRW_INSTALL_WINDOWS_H

#include "digby.h"
#include "shim_windows.h"
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

struct local_package {
    const wchar_t *name;
    const wchar_t *version;
    const wchar_t *archive;
    const wchar_t *digest;
    const wchar_t *entry;
};

/* Removes a private staging directory tree without traversing reparse points. */
bool brw_remove_git_stage(const wchar_t *stage);

/* Input strings are borrowed for the duration of the call. Errors are static. */
const char *brw_validate_package(const struct local_package *package);
const char *brw_validate_lifecycle(enum digby_action action, const struct local_package *package);
const char *brw_install_package(const struct local_package *package,
                                const struct digby_plan *plan, size_t *failed_step);
const char *brw_run_lifecycle(enum digby_action action, const struct local_package *package,
                             const struct digby_plan *plan, size_t *failed_step);

bool brw_package_component(const wchar_t *name);
/* Resolves a validated package's public command path in the supplied bin root. */
bool brw_command_entry_path(const wchar_t *bin, const wchar_t *name, wchar_t path[BRW_PATH_CAP]);
const char *brw_validate_source_package(const struct local_package *package, bool executable);
int brw_list_installed(bool json);
int brw_recover(bool json, bool preview);
enum { BRW_DEPENDENCIES_CAP = 1024, BRW_PACKAGE_NAME_CAP = 128 };
/* Comma-separated package names. NULL and empty represent no dependencies. */
bool brw_dependency_names(const wchar_t *names);
/* Advances a cursor over validated names, returning false at the end. */
bool brw_next_dependency(const wchar_t **names, wchar_t name[BRW_PACKAGE_NAME_CAP]);
bool brw_is_installed(const wchar_t *name);
/* Non-reentrant. A successful begin must be paired with end in the same process. */
const char *brw_begin_package_changes(void);
void brw_end_package_changes(void);
/* Hold the console cancellation handler across the whole source install. */
bool brw_begin_source_install(void);
void brw_end_source_install(void);
const char *brw_check_uninstall(const wchar_t *name, wchar_t dependent[BRW_PACKAGE_NAME_CAP]);
/* Borrows validated inputs for the call under the package mutation lock. Closes
   all stage handles. Returns NULL or a static error. If supplied, failed_step
   is zero-based and initialized to zero before execution. */
const char *brw_install_with_dependencies(const struct local_package *package, bool executable,
                                          const wchar_t *dependencies,
                                          const struct digby_plan *plan, size_t *failed_step);
/* Consumes an open binary payload positioned at offset zero. Caller holds the
   package mutation lock and has validated the package as a source package. */
const char *brw_install_stream(const struct local_package *package, bool executable, FILE *payload);
/* Validates and materializes a pinned artifact under the package mutation lock.
   Inputs are borrowed. On success target receives its absolute version entry.
   A non-NULL default_changed requests default publication and receives true
   only when it succeeds in changing the public entry. */
const char *brw_use_locked_package(const struct local_package *package, bool executable,
                                   const wchar_t *dependencies, bool *default_changed,
                                   wchar_t target[BRW_PATH_CAP]);
/* Read-only check of the public shim and its recorded version and entry. */
const char *brw_default_matches(const wchar_t *name, const wchar_t *version,
                                const wchar_t *entry, bool *matches);
int brw_report_plan(bool json, bool preview, const char *status,
                    const struct digby_plan *plan, const char *error, size_t step);

#endif
