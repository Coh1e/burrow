#ifndef BRW_ARCHIVE_CLI_H
#define BRW_ARCHIVE_CLI_H

#define BRW_UNPACK_USAGE "brw unpack ARCHIVE PATH --sha256 DIGEST --workspace DIRECTORY [--dry-run] [--json]"
int brw_unpack_command(int argc, const char *const *argv);

#endif
