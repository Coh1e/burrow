#ifndef BRW_INSTALL_WINDOWS_H
#define BRW_INSTALL_WINDOWS_H

#include "digby.h"
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
const char *brw_install_package(const struct local_package *package,
                                const struct digby_plan *plan, size_t *failed_step);

#endif
