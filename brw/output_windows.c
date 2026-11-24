#include "source_windows.h"

void brw_json_string(const wchar_t *value)
{
    putchar('"');
    for (; *value; ++value) {
        unsigned ch = *value;
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 32 || ch > 126)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}
