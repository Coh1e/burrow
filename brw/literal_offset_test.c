#include "literal_offset.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char exact[4];
    if (brw_format_offset(197, exact, sizeof exact) || memcmp(exact, "197", sizeof exact)) {
        fputs("exact capacity did not produce the decimal offset\n", stderr);
        return 1;
    }
    char guarded[] = "xx!";
    if (brw_format_offset(0, guarded, 2) || memcmp(guarded, "0\0!", sizeof guarded)) {
        fputs("zero offset exceeded the supplied buffer or lacked a terminator\n", stderr);
        return 1;
    }
    char small[] = "xxx";
    if (brw_format_offset(197, small, 3) != 1 || brw_format_offset(0, small, 0) != 1 ||
        brw_format_offset(0, NULL, 2) != 1 || memcmp(small, "xxx", sizeof small)) {
        fputs("invalid buffer was accepted or overwritten\n", stderr);
        return 1;
    }
    char maximum[sizeof(size_t) * CHAR_BIT + 1], expected[sizeof maximum];
    int digits = snprintf(expected, sizeof expected, "%zu", (size_t)SIZE_MAX);
    if (digits < 1 || (size_t)digits >= sizeof expected ||
        brw_format_offset(SIZE_MAX, maximum, (size_t)digits + 1) ||
        memcmp(maximum, expected, (size_t)digits + 1)) {
        fputs("maximum size_t offset was truncated\n", stderr);
        return 1;
    }
    return 0;
}
