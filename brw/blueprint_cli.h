#ifndef BRW_BLUEPRINT_CLI_H
#define BRW_BLUEPRINT_CLI_H

#define BRW_BLUEPRINT_USAGE "brw blueprint preview|apply BUNDLE --sha256 DIGEST --workspace DIRECTORY [--dry-run] [--json]"
/* argv begins with preview or apply. All strings are borrowed UTF-8. */
int brw_blueprint_command(int argc, const char *const *argv);
#ifdef _WIN32
#include <wchar.h>
int brw_blueprint_wcommand(int argc, wchar_t **argv);
#endif
#endif
