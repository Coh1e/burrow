#include "git_worktree.h"
#include <git2.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char commit[BRW_COMMIT_CAP];
    if (argc < 3 || git_libgit2_init() < 0) return 2;
    const char *error = "invalid-arguments";
    if (argc == 4 && !strcmp(argv[1], "clone")) error = brw_clone_staged(argv[2], argv[3], commit);
    if (argc == 3 && !strcmp(argv[1], "pull")) error = brw_pull_worktree(argv[2], false, commit);
    puts(error ? error : commit);
    git_libgit2_shutdown();
    return error != NULL;
}
