#ifndef BRW_BLUEPRINT_EDIT_H
#define BRW_BLUEPRINT_EDIT_H
#include "blueprint.h"
#include "literal.h"
#include <string.h>

enum { BP_TEXT_LIMIT = 16 * 1024 * 1024 };
bool bp_valid_utf8(const char *text, size_t size);

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
/* Compare validated paths differing only in ASCII case, using their actual
   parent directories. No filesystem entry may be created during preflight. */
enum bp_output_match { BP_DISTINCT_OUTPUTS, BP_SAME_OUTPUT, BP_UNKNOWN_CASE };
const char *bp_compare_output_case(const char *workspace, const char *a,
                                   const char *b, enum bp_output_match *match);
const char *bp_validate_file_operands(const struct bp_step *s);
const char *bp_replace_text(FILE *stage, const struct bp_step *s, const char *text, size_t size);
/* Validate the half-open segment between external states. Indices refer to plan. */
const char *bp_validate_text_range(const char *workspace, const struct bp_plan *plan,
                                   size_t start, size_t end, size_t *failed_step);

#endif
