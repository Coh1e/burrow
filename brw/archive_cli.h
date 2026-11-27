#ifndef BRW_ARCHIVE_CLI_H
#define BRW_ARCHIVE_CLI_H

#define BRW_UNPACK_USAGE "brw unpack ARCHIVE PATH --sha256 DIGEST --workspace DIRECTORY [--dry-run] [--json]"
#define BRW_PACK_USAGE "brw pack ARCHIVE INPUT... --format tar|tar.gz|zip --workspace DIRECTORY [--dry-run] [--json]"
int brw_unpack_command(int argc, const char *const *argv);
int brw_pack_command(int argc, const char *const *argv);
/* Report an unpack error for the given UTF-8 source and workspace destination. */
void brw_unpack_diagnostic(const char *payload, const char *destination, const char *error);

#endif
