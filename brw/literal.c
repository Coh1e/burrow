#include "literal.h"
#include <string.h>
#ifdef BRW_ENABLE_CPP_MODULE
#include "literal_offset.h"
#include <limits.h>
#endif

static int write_offset(FILE *output, size_t offset)
{
#ifdef BRW_ENABLE_CPP_MODULE
    char text[sizeof offset * CHAR_BIT + 1];
    return brw_format_offset(offset, text, sizeof text) || fprintf(output, "%s\n", text) < 0;
#else
    return fprintf(output, "%zu\n", offset) < 0;
#endif
}

int brw_literal(FILE *output, const char *text, const char *needle,
                const char *replacement, const size_t *expected)
{
    if (!output || !text || !needle || !*needle)
        return 1;
    size_t length = strlen(needle), count = 0;
    const char *cursor = text;
    const char *match;
    while ((match = strstr(cursor, needle)) != NULL) {
        if (replacement) {
            size_t prefix = (size_t)(match - cursor);
            if (fwrite(cursor, 1, prefix, output) != prefix || fputs(replacement, output) < 0)
                return 1;
        } else if (write_offset(output, (size_t)(match - text))) {
            return 1;
        }
        cursor = match + length;
        ++count;
    }
    if (expected ? count != *expected : count == 0) {
        fprintf(stderr, "matched %zu occurrences\n", count);
        return 1;
    }
    return replacement && fputs(cursor, output) < 0;
}
