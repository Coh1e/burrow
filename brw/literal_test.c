#include "literal.h"
#include <string.h>

int main(void)
{
    const size_t two = 2;
    const char text[] = "a\xe6\x97\xa7" "a\xe6\x97\xa7";
    FILE *out = tmpfile();
    if (!out)
        return 1;
    int failed = brw_literal(out, text, "\xe6\x97\xa7", NULL, &two);
    rewind(out);
    char bytes[8] = {0};
    failed |= fread(bytes, 1, sizeof(bytes), out) != 4 || strcmp(bytes, "1\n5\n");
    failed |= !brw_literal(out, text, "", NULL, NULL);
    failed |= !brw_literal(NULL, text, "a", NULL, NULL);
    return fclose(out) != 0 || failed;
}
