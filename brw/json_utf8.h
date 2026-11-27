#ifndef BRW_JSON_UTF8_H
#define BRW_JSON_UTF8_H

#include <stdio.h>

static inline void quoted(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') putchar('\\');
        if (*p < 32) printf("\\u%04x", (unsigned)*p);
        else putchar(*p);
    }
    putchar('"');
}

#endif
