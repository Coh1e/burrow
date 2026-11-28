#include "git_windows.h"
#include "git_worktree.h"
#include "install_windows.h"
#include "shim_windows.h"
#include <windows.h>
#include <git2.h>
#include <stdio.h>
#include <stdlib.h>

struct git_command {
    const wchar_t *source;
    const wchar_t *directory;
    char *commit;
    bool preview;
    brw_git_scope scope;
    void *scope_context;
};

static char *utf8(const wchar_t *value)
{
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    char *text = size ? malloc((size_t)size) : NULL;
    if (text && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text, size, NULL, NULL)) {
        free(text);
        return NULL;
    }
    return text;
}

static const char *clone(struct git_command *cmd)
{
    if (GetFileAttributesW(cmd->directory) != INVALID_FILE_ATTRIBUTES) return "destination-exists";
    if (GetLastError() != ERROR_FILE_NOT_FOUND) return "destination-unavailable";
    if (cmd->preview) return NULL;
    wchar_t stage[BRW_PATH_CAP];
    int length = swprintf(stage, BRW_PATH_CAP, L"%ls.brw-%lu-%llu", cmd->directory,
                          GetCurrentProcessId(), GetTickCount64());
    if (length < 0 || length >= BRW_PATH_CAP || !CreateDirectoryW(stage, NULL))
        return "create-stage-failed";
    wchar_t absolute[BRW_PATH_CAP];
    const wchar_t *source = cmd->source;
    if (GetFileAttributesW(source) != INVALID_FILE_ATTRIBUTES) {
        DWORD n = GetFullPathNameW(source, BRW_PATH_CAP, absolute, NULL);
        if (n && n < BRW_PATH_CAP) source = absolute;
    }
    char *url = utf8(source), *path = utf8(stage);
    const char *error = url && path ? brw_clone_staged(url, path, cmd->commit) : "invalid-path";
    free(path);
    free(url);
    if (!error && !MoveFileExW(stage, cmd->directory, MOVEFILE_WRITE_THROUGH))
        error = "publish-clone-failed";
    if (error && GetFileAttributesW(stage) != INVALID_FILE_ATTRIBUTES && !brw_remove_git_stage(stage))
        return "clone-stage-cleanup-failed";
    return error;
}

static const char *git_step(void *context, enum digby_operation op)
{
    struct git_command *cmd = context;
    if (op == DIGBY_GIT_CLONE) return clone(cmd);
    char *path = utf8(cmd->directory);
    const char *error = path ? brw_pull_worktree_scoped(path, cmd->preview, cmd->commit,
                                                        cmd->scope, cmd->scope_context) : "invalid-path";
    free(path);
    return error;
}

const char *brw_git_worktree(const wchar_t *source, const wchar_t *destination,
                            bool preview, char commit[BRW_COMMIT_CAP], brw_git_scope scope, void *context)
{
    commit[0] = '\0';
    struct git_command cmd = {source, destination, commit, preview, scope, context};
    const struct digby_plan plan = {{source ? DIGBY_GIT_CLONE : DIGBY_GIT_PULL}, 1};
    if (git_libgit2_init() < 0) return "git-init-failed";
    const char *error = digby_execute(&plan, git_step, &cmd, NULL);
    git_libgit2_shutdown();
    return error;
}

int brw_git_command(size_t count, const wchar_t **args, bool json, bool preview)
{
    bool cloning = count == 3 && !wcscmp(args[0], L"clone");
    if (!cloning && !(count == 2 && !wcscmp(args[0], L"pull"))) {
        fputs("usage: " BRW_GIT_USAGE "\n", stderr);
        return 2;
    }
    char commit[BRW_COMMIT_CAP];
    wchar_t directory[BRW_PATH_CAP];
    const wchar_t *source = cloning ? args[1] : NULL;
    const wchar_t *destination = args[count - 1];
    DWORD n = GetFullPathNameW(destination, BRW_PATH_CAP, directory, NULL);
    const struct digby_plan plan = {{cloning ? DIGBY_GIT_CLONE : DIGBY_GIT_PULL}, 1};
    const char *operation = digby_operation_name(plan.steps[0]);
    const char *error = NULL;
    if (!*destination || !n || n >= BRW_PATH_CAP || (cloning && !*source))
        error = "invalid-git-input";
    else error = brw_git_worktree(source, directory, preview, commit, NULL, NULL);
    if (error) return brw_report_plan(json, preview, "failed", &plan, error, 1);
    if (json) {
        printf("{\"status\":\"%s\",\"operations\":[\"%s\"]",
               preview ? "planned" : cloning ? "cloned" : "updated", operation);
        if (*commit) printf(",\"commit\":\"%s\"", commit);
        if (preview) fputs(",\"pending\":[\"remote-revision\"]", stdout);
        puts("}");
    } else {
        printf("%s %s\n", operation, commit);
        if (preview) puts("Remote revision pending.");
    }
    return 0;
}
