#ifndef BRW_BLUEPRINT_EDIT_H
#define BRW_BLUEPRINT_EDIT_H
#include "blueprint.h"
#include "literal.h"
#include <string.h>

enum { BP_TEXT_LIMIT = 16 * 1024 * 1024 };

static inline bool bp_env_name(const char *name)
{
    if (!name || !*name) return false;
    for (const char *p = name; *p; ++p)
        if (*p != '_' && !(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(p != name && *p >= '0' && *p <= '9')) return false;
    return true;
}

/* Platform reader. On success returns an owned, NUL-terminated snapshot. */
const char *bp_read_workspace_text(const char *workspace, const char *relative,
                                   char **text, size_t *size);
const char *bp_validate_file_operands(const struct bp_step *s);
const char *bp_replace_text(FILE *stage, const struct bp_step *s, const char *text, size_t size);
#endif
