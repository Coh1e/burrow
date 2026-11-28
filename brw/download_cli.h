#ifndef BRW_DOWNLOAD_CLI_H
#define BRW_DOWNLOAD_CLI_H

#define BRW_DOWNLOAD_USAGE "brw download URL PATH --sha256 DIGEST --workspace DIRECTORY [--dry-run] [--json]"
int brw_download_command(int argc, const char *const *argv);
/* Print a stable diagnostic and a recovery hint for a UTF-8 destination. */
void brw_download_diagnostic(const char *destination, const char *error);

#endif
