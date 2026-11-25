#include "git_worktree.h"
#include <git2.h>
#include <string.h>

static const char *checked_commit(git_repository *repo, char commit[BRW_COMMIT_CAP])
{
    git_oid id;
    if (git_reference_name_to_id(&id, repo, "HEAD") < 0) return "missing-head";
    git_oid_tostr(commit, BRW_COMMIT_CAP, &id);
    return NULL;
}

const char *brw_clone_staged(const char *source, const char *stage, char commit[BRW_COMMIT_CAP])
{
    git_repository *repo = NULL;
    git_config *config = NULL;
    if (!source || !*source || !stage || !*stage || !commit) return "invalid-git-input";
    const char *error = "clone-failed";
    git_clone_options options;
    git_clone_options_init(&options, GIT_CLONE_OPTIONS_VERSION);
    options.local = GIT_CLONE_NO_LOCAL;
    if (!git_clone(&repo, source, stage, &options)) {
        error = checked_commit(repo, commit);
        if (!error && (git_repository_config(&config, repo) < 0 ||
                       git_config_set_bool(config, "brw.managed", 1) < 0))
            error = "mark-worktree-failed";
    }
    git_config_free(config);
    git_repository_free(repo);
    return error;
}

static const char *clean_worktree(git_repository *repo)
{
    git_status_options opts;
    git_status_list *list = NULL;
    if (git_repository_state(repo) != GIT_REPOSITORY_STATE_NONE) return "git-operation-in-progress";
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    if (git_status_list_new(&list, repo, &opts) < 0) return "status-failed";
    size_t count = git_status_list_entrycount(list);
    git_status_list_free(list);
    return count ? "dirty-worktree" : NULL;
}

static void checkout_progress(const char *path, size_t completed, size_t total, void *payload)
{
    (void)path;
    (void)total;
    *(size_t *)payload = completed;
}

static const char *advance(git_repository *repo, git_reference *head, const git_oid *target)
{
    const git_oid *previous = git_reference_target(head);
    if (git_oid_equal(previous, target)) return NULL;
    int descends = git_graph_descendant_of(repo, target, previous);
    if (descends < 0) return "check-ancestry-failed";
    if (!descends) return "non-fast-forward";

    git_object *next = NULL, *old = NULL, *baseline = NULL;
    size_t completed = 0;
    git_checkout_options checkout;
    git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION);
    checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
    checkout.progress_cb = checkout_progress;
    checkout.progress_payload = &completed;
    git_transaction *tx = NULL;
    git_oid current;
    const char *branch = git_reference_name(head);
    const char *error = "lock-worktree-failed";
    if (git_transaction_new(&tx, repo) < 0 || git_transaction_lock_ref(tx, "HEAD") < 0 ||
        git_transaction_lock_ref(tx, branch) < 0) goto done;
    error = "head-changed";
    if (git_branch_is_head(head) != 1 || git_reference_name_to_id(&current, repo, branch) < 0 ||
        !git_oid_equal(&current, previous)) goto done;
    error = clean_worktree(repo);
    if (error) goto done;
    error = "update-branch-failed";
    if (git_transaction_set_target(tx, branch, target, NULL, "brw fast-forward") < 0) goto done;
    error = "checkout-failed";
    if (git_object_lookup(&next, repo, target, GIT_OBJECT_COMMIT) < 0 ||
        git_object_lookup(&old, repo, previous, GIT_OBJECT_COMMIT) < 0 ||
        git_object_peel(&baseline, next, GIT_OBJECT_TREE) < 0) goto done;
    int rc = git_checkout_tree(repo, next, &checkout);
    if (rc == 0 && git_transaction_commit(tx) == 0) {
        error = NULL;
        goto done;
    }
    if (rc == GIT_ELOCKED) error = "index-locked";
    if (rc == 0) error = "update-branch-failed";
    if (completed || rc == 0) {
        /* Checkout writes the index last. If it failed, the on-disk index
           still describes the old tree. Undo file updates without touching it. */
        if (rc < 0) checkout.checkout_strategy |= GIT_CHECKOUT_DONT_UPDATE_INDEX;
        checkout.baseline = (git_tree *)baseline;
        if (git_checkout_tree(repo, old, &checkout) < 0) error = "checkout-recovery-failed";
    }
done:
    git_transaction_free(tx);
    git_object_free(baseline);
    git_object_free(old);
    git_object_free(next);
    return error;
}

const char *brw_pull_worktree(const char *path, bool preview, char commit[BRW_COMMIT_CAP])
{
    git_repository *repo = NULL;
    git_config *config = NULL;
    git_config *local = NULL;
    git_reference *head = NULL;
    git_remote *remote = NULL;
    git_buf remote_name = GIT_BUF_INIT, merge = GIT_BUF_INIT;
    const git_remote_head **refs;
    size_t count;
    int managed = 0;
    if (!path || !*path || !commit) return "invalid-git-input";
    const char *error = "unmanaged-worktree";
    if (git_repository_open_ext(&repo, path, GIT_REPOSITORY_OPEN_NO_SEARCH, NULL) < 0 ||
        git_repository_is_bare(repo) || git_repository_config(&config, repo) < 0 ||
        git_config_open_level(&local, config, GIT_CONFIG_LEVEL_LOCAL) < 0 ||
        git_config_get_bool(&managed, local, "brw.managed") < 0 || !managed) goto done;
    error = "missing-upstream";
    if (git_repository_head(&head, repo) < 0 || !git_reference_is_branch(head) ||
        git_branch_upstream_remote(&remote_name, repo, git_reference_name(head)) < 0 ||
        git_branch_upstream_merge(&merge, repo, git_reference_name(head)) < 0 ||
        git_remote_lookup(&remote, repo, remote_name.ptr) < 0) goto done;
    error = clean_worktree(repo);
    if (error) goto done;
    if (!preview) {
        /* Use the advertised branch tip from this fetch, even if a stale
           tracking reference survived deletion of the upstream branch. */
        git_strarray specs = {&merge.ptr, 1};
        error = "fetch-failed";
        if (git_remote_fetch(remote, &specs, NULL, "brw pull") < 0 ||
            git_remote_ls(&refs, &count, remote) < 0) goto done;
        error = "missing-upstream";
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(merge.ptr, refs[i]->name)) continue;
            error = advance(repo, head, &refs[i]->oid);
            break;
        }
        if (error) goto done;
    }
    error = checked_commit(repo, commit);
done:
    git_remote_free(remote);
    git_reference_free(head);
    git_buf_dispose(&merge);
    git_buf_dispose(&remote_name);
    git_config_free(local);
    git_config_free(config);
    git_repository_free(repo);
    return error;
}
