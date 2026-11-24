#ifndef BRW_LITERAL_H
#define BRW_LITERAL_H

#include <stdio.h>

/* All strings are borrowed and NUL-terminated. A NULL replacement prints
   byte offsets; otherwise write the replaced text. Return zero on success
   or nonzero on invalid input, count mismatch, or write failure. The caller
   owns output and discards it on error. A NULL expected count requires at
   least one match. */
int brw_literal(FILE *output, const char *text, const char *needle,
                const char *replacement, const size_t *expected);
#endif
