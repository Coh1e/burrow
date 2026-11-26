#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sibling_version(const char *program)
{
    size_t capacity = strlen(program) + sizeof ".sibling";
    char *path = malloc(capacity);
    if (!path) return EOF;
    snprintf(path, capacity, "%s.sibling", program);
    FILE *file = fopen(path, "rb");
    free(path);
    if (!file) return EOF;
    int version = fgetc(file);
    fclose(file);
    return version;
}

int main(int argc, char **argv)
{
    if (argc == 1) return puts(TOOL_VERSION) == EOF;
    int version = TOOL_VERSION[0];
    if (argc == 3 && !strcmp(argv[1], "--sibling")) version = sibling_version(argv[0]);
    else if (argc != 2) return 2;
    if (version == EOF) return 1;
    FILE *output = fopen(argv[argc - 1], "wb");
    if (!output) return 1;
    int result = fputc(version, output);
    return fclose(output) != 0 || result == EOF;
}
