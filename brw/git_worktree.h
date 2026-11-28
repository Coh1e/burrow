#ifndef BRW_GIT_WORKTREE_H
#define BRW_GIT_WORKTREE_H
#include <stdbool.h>

/* UTF-8 inputs are borrowed. The caller initializes libgit2 and owns the
   private staging directory. On success commit receives the checked-out SHA-1. */
enum { BRW_COMMIT_CAP = 41 };
const char *brw_clone_staged(const char *source, const char *stage, char commit[BRW_COMMIT_CAP]);
const char *brw_pull_worktree(const char *path, bool preview, char commit[BRW_COMMIT_CAP]);
/* The validator borrows libgit2's resolved paths before any status or fetch. */
typedef bool (*brw_git_scope)(void *context, const char *worktree, const char *metadata,
                              const char *common);
const char *brw_pull_worktree_scoped(const char *path, bool preview, char commit[BRW_COMMIT_CAP],
                                    brw_git_scope scope, void *context);
#endif
