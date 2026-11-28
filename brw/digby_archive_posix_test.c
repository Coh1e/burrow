#define _POSIX_C_SOURCE 200809L
#include "digby_archive_io.h"
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The workspace is private to this test. Its only child is the opened stage. */
static char *stage_name(const char *pattern)
{
    glob_t paths = {0};
    char *name = NULL;
    if (!glob(pattern, 0, NULL, &paths) && paths.gl_pathc == 1)
        name = strdup(paths.gl_pathv[0]);
    globfree(&paths);
    return name;
}

static bool rollback_preserves_unrecorded_file(const struct bp_directory *workspace)
{
    struct digby_archive_stage *stage;
    FILE *file;
    if (digby_archive_begin(workspace, "output", &stage) ||
        digby_archive_entry(stage, "nested/owned", false, false, &file) || fclose(file)) return false;
    char *name = stage_name(".brw-unpack-*");
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

static bool pack_preserves_other_writer(const struct bp_directory *workspace)
{
    struct digby_pack_stage *first, *second;
    FILE *one, *two;
    if (digby_pack_begin(workspace, "archive.tar", &first, &one) ||
        digby_pack_begin(workspace, "archive.tar", &second, &two) ||
        fputs("first", one) < 0 || fputs("second", two) < 0 ||
        digby_pack_finish(second, true)) return false;
    const char *error = digby_pack_finish(first, true);
    if (!error || strcmp(error, "destination-exists")) return false;
    FILE *result = fopen("archive.tar", "rb");
    char bytes[8];
    if (!result || fread(bytes, 1, sizeof bytes, result) != 6 || memcmp(bytes, "second", 6) ||
        fclose(result) || unlink("archive.tar")) return false;
    /* The final rmdir in main also checks both private stages were removed. */
    return true;
}

static bool renamed_stage_is_preserved(const struct bp_directory *workspace)
{
    struct digby_archive_stage *stage;
    if (digby_archive_begin(workspace, "output", &stage)) return false;
    char *name = stage_name(".brw-unpack-*");
    if (!name || rename(name, "retained")) return false;
    free(name);
    const char *error = digby_archive_finish(stage, false);
    return error && !strcmp(error, "stage-changed") && !rmdir("retained");
}

static bool pack_reports_moved_directory(const struct bp_directory *workspace)
{
    struct digby_pack_stage *stage;
    FILE *stream;
    if (digby_pack_begin(workspace, "output", &stage, &stream) || fputs("ready", stream) < 0) return false;
    char *name = stage_name(".brw-pack-*");
    struct stat info;
    bool moved = name && !lstat(name, &info) && S_ISDIR(info.st_mode) && !(info.st_mode & 0077) &&
                 !rename(name, "retained");
    free(name);
    const char *error = digby_pack_finish(stage, true);
    return moved && error && !strcmp(error, "stage-changed") && access("output", F_OK) != 0 &&
           !unlink("retained/archive") && !rmdir("retained");
}

static bool pack_root_is_pinned(void)
{
    if (mkdir("work", 0700) || mkdir("work/products", 0700)) return false;
    struct bp_directory *workspace = bp_directory_open("work");
    if (!workspace) return false;
    /* Rotate the workspace before checking and opening the archive destination. */
    const char *relative = "products/result";
    struct digby_pack_stage *stage;
    FILE *stream;
    bool opened = !rename("work", "held") && !mkdir("work", 0700) &&
                  !digby_archive_destination(workspace, relative) &&
                  !digby_pack_begin(workspace, relative, &stage, &stream);
    bp_directory_close(workspace);
    if (!opened) return false;
    bool written = fputs("held", stream) >= 0;
    if (digby_pack_finish(stage, written) || !written) return false;
    return !unlink("held/products/result") && !rmdir("held/products") &&
           !rmdir("held") && !rmdir("work");
}

int main(void)
{
    char root[] = "/tmp/brw-archive-rollback-XXXXXX";
    if (!mkdtemp(root) || chdir(root)) return 1;
    struct bp_directory *workspace = bp_directory_open(".");
    if (!workspace) return 1;
    bool ok = rollback_preserves_unrecorded_file(workspace) && renamed_stage_is_preserved(workspace) &&
              pack_root_is_pinned() && pack_preserves_other_writer(workspace) &&
              pack_reports_moved_directory(workspace);
    bp_directory_close(workspace);
    if (!ok) {
        fprintf(stderr, "Archive staging contract failed in %s\n", root);
        return 1;
    }
    return chdir("/") || rmdir(root);
}
