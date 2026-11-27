#define _POSIX_C_SOURCE 200809L
#include "digby_archive_io.h"
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The workspace is private to this test. Its only child is the opened stage. */
static char *stage_name(void)
{
    glob_t paths = {0};
    char *name = NULL;
    if (!glob(".brw-unpack-*", 0, NULL, &paths) && paths.gl_pathc == 1)
        name = strdup(paths.gl_pathv[0]);
    globfree(&paths);
    return name;
}

static bool rollback_preserves_unrecorded_file(void)
{
    struct digby_archive_stage *stage;
    FILE *file;
    if (digby_archive_begin(".", "output", &stage) ||
        digby_archive_entry(stage, "nested/owned", false, false, &file) || fclose(file)) return false;
    char *name = stage_name();
    if (!name || chdir(name) || chdir("nested")) return false;
    FILE *note = fopen("note", "w");
    if (!note || fputs("keep", note) < 0 || fclose(note)) return false;
    const char *error = digby_archive_finish(stage, false);
    bool ok = error && !strcmp(error, "cleanup-stage") && access("owned", F_OK) != 0;
    note = fopen("note", "r");
    char text[8];
    if (!note || !fgets(text, sizeof text, note) || strcmp(text, "keep") || fclose(note)) return false;
    if (unlink("note") || chdir("..") || rmdir("nested") || chdir("..") || rmdir(name)) return false;
    free(name);
    return ok;
}

static bool renamed_stage_is_preserved(void)
{
    struct digby_archive_stage *stage;
    if (digby_archive_begin(".", "output", &stage)) return false;
    char *name = stage_name();
    if (!name || rename(name, "retained")) return false;
    free(name);
    const char *error = digby_archive_finish(stage, false);
    return error && !strcmp(error, "stage-changed") && !rmdir("retained");
}

int main(void)
{
    char root[] = "/tmp/brw-archive-rollback-XXXXXX";
    if (!mkdtemp(root) || chdir(root)) return 1;
    if (!rollback_preserves_unrecorded_file() || !renamed_stage_is_preserved()) {
        fprintf(stderr, "Rollback contract failed in %s\n", root);
        return 1;
    }
    return chdir("/") || rmdir(root);
}
