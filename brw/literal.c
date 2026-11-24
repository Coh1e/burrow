#include "literal.h"
#include <string.h>

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
        } else if (fprintf(output, "%zu\n", (size_t)(match - text)) < 0) {
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
