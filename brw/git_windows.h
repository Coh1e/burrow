#ifndef BRW_GIT_WINDOWS_H
#define BRW_GIT_WINDOWS_H
#include <stdbool.h>
#include <wchar.h>
#include "git_worktree.h"
#define BRW_GIT_USAGE "brw git clone SOURCE DIRECTORY | brw git pull DIRECTORY [--dry-run] [--json]"
/* An absolute destination and optional clone source are borrowed for the call.
   A NULL source selects pull. The caller owns the commit buffer. */
const char *brw_git_worktree(const wchar_t *source, const wchar_t *destination,
                            bool preview, char commit[BRW_COMMIT_CAP], brw_git_scope scope, void *context);
int brw_git_command(size_t count, const wchar_t **args, bool json, bool preview);
#endif
