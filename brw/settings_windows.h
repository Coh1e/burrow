#ifndef BRW_SETTINGS_WINDOWS_H
#define BRW_SETTINGS_WINDOWS_H
#include <stdbool.h>
#include <wchar.h>

/* Borrows the CLI arguments. Session changes live only in this process and its child. */
int brw_settings_command(int argc, wchar_t **argv);
/* Reports a pending settings transaction to other package mutations. */
const char *brw_settings_pending(void);
/* Caller holds the package mutation lock. A later external edit is preserved. */
const char *brw_settings_recover(bool preview, bool *pending, bool *preserved);
#endif
