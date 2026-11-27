#ifndef BRW_SSH_COMMAND_WINDOWS_H
#define BRW_SSH_COMMAND_WINDOWS_H

#include <wchar.h>

#define BRW_SSH_USAGE "brw ssh USER@HOST --allow-target USER@HOST:PORT --identity FILE --known-hosts FILE" \
                      " [--port PORT] [--timeout SECONDS] [--dry-run] [--json] -- COMMAND"

/* argv starts at USER@HOST. All arguments belong to the caller. */
int brw_ssh_command(int argc, wchar_t **argv);

#endif
