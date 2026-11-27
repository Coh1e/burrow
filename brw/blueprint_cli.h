#ifndef BRW_BLUEPRINT_CLI_H
#define BRW_BLUEPRINT_CLI_H

#define BRW_BLUEPRINT_USAGE "brw blueprint lock BUNDLE --sha256 DIGEST --workspace DIRECTORY [--json]\n" \
    "       brw blueprint preview|apply BUNDLE --sha256 DIGEST --workspace DIRECTORY" \
    " [--allow-target USER@HOST:PORT --identity FILE --known-hosts FILE] [--dry-run] [--json]"
/* argv begins with lock, preview or apply. All strings are borrowed UTF-8. */
#ifndef _WIN32
int bp_source_add(const char *name, const char *catalog);
#endif
int brw_blueprint_command(int argc, const char *const *argv);
#ifdef _WIN32
#include <wchar.h>
int brw_utf8_wcommand(int argc, wchar_t **argv, int (*command)(int, const char *const *));
#endif
#endif
