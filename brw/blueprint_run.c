#include "blueprint_edit.h"
#include "blueprint_lock.h"
#include "digby_archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct runtime_output {
    struct bp_output_file file;
    char *value;
};

static const char *read_value(FILE *stream, char **value)
{
    if (!stream) return "missing-output";
    char bytes[4097];
    size_t n = fread(bytes, 1, sizeof bytes, stream);
    if (ferror(stream) || n == sizeof bytes || memchr(bytes, 0, n))
        return "invalid-output";
    if (n && bytes[n - 1] == '\n') {
        --n;
        if (n && bytes[n - 1] == '\r') --n;
    }
    if (!n) return "missing-output";
    if (memchr(bytes, '\n', n) || memchr(bytes, '\r', n) || !bp_valid_utf8(bytes, n))
        return "invalid-output";
    *value = malloc(n + 1);
    if (!*value) return "out-of-memory";
    memcpy(*value, bytes, n);
    (*value)[n] = 0;
    return NULL;
}

static const char *command(const struct bp_lock *lock, const char *workspace, const struct bp_step *step,
                           struct runtime_output *outputs, unsigned long *exit_code, bool *started)
{
    *started = false;
    struct bp_step command = *step;
    char *args[BP_MAX_ARGS + 1] = {0};
    command.args = args;
    for (size_t i = 0; i < step->argc; ++i) {
        args[i] = step->args[i];
        struct bp_binding binding = step->bindings[i];
        if (binding.output) {
            struct runtime_output *output = &outputs[binding.output - 1];
            if (binding.writer && !output->file.path && !bp_create_output_file(&output->file))
                return "output-stage";
            args[i] = binding.writer ? output->file.path : output->value;
        }
    }
    char *program = NULL, *child_path = NULL;
    const char *error = NULL;
    if (step->tool) {
        error = bp_use_tool(lock, step, &program, &child_path);
        command.program = program;
        command.child_path = child_path;
    }
    if (!error) error = bp_run_command(workspace, &command, true, exit_code, started);
    free(child_path);
    free(program);
    return error;
}

static struct digby_archive_plan archive_plan(const char *workspace, const struct bp_step *step)
{
    return (struct digby_archive_plan){
        .data = step->payload && !step->payload->module ? step->payload->data : NULL,
        .size = step->payload ? step->payload->size : 0,
        .workspace = workspace, .destination = step->path
    };
}

static const char *validate_step(const char *workspace, const struct bp_step *step)
{
    if (!step->tool) return bp_validate_step(workspace, step);
    struct bp_step command = *step;
    command.program = bp_version_entry(step->package);
    if (!command.program) return "data-directory";
    const char *error = bp_validate_step(workspace, &command);
    free(command.program);
    return error;
}

static struct digby_ssh_result run_ssh(const struct bp_plan *plan, const struct bp_ssh *step,
                                       bool preview)
{
    const struct bp_ssh_options *options = &plan->ssh_options;
    if (!plan->ssh_backend)
        return (struct digby_ssh_result){DIGBY_SSH_REJECTED, "missing-capability", -1};
    if (!options->allowed_target)
        return (struct digby_ssh_result){DIGBY_SSH_REJECTED, "unauthorized-target", -1};
    const struct digby_ssh_request request = {
        .target = step->target, .port = step->port, .command = step->command,
        .timeout_seconds = step->timeout_seconds,
        .identity_file = options->identity_file, .known_hosts = options->known_hosts
    };
    if (preview) return digby_ssh_run(&request, options->allowed_target, true, NULL, NULL);
    return plan->ssh_backend(&request, options->allowed_target, false);
}

static const char *validate_ssh_plan(struct bp_plan *plan, bool preview, struct bp_run_result *result)
{
    const char *error = NULL;
    for (size_t i = 0; i < plan->count; ++i) {
        struct bp_step *step = &plan->steps[i];
        if (step->op != BP_SSH) continue;
        step->ssh->reported = false;
        if (error) continue;
        step->ssh->result = run_ssh(plan, step->ssh, true);
        error = step->ssh->result.error;
        step->ssh->reported = preview || error;
        if (error) result->failure_source = step->source;
    }
    return error;
}

static bool needs_output(const struct bp_step *command)
{
    for (size_t i = 0; i < command->argc; ++i)
        if (command->bindings[i].output) return true;
    return false;
}

static const char *observe_state(const struct bp_lock *lock, const char *workspace, struct bp_state *state,
                                 struct runtime_output *outputs, bool *started)
{
    struct bp_step check = state->check;
    if (state->required) {
        free(state->detected);
        state->detected = NULL;
        state->has_exit_code = false;
        state->status = BP_UNCHECKED;
        *started = false;
        check.capture_stdout = bp_temp_stream();
        if (!check.capture_stdout) return "output-stage";
    }
    unsigned long code = 0;
    const char *error = command(lock, workspace, &check, outputs, &code, started);
    if (!error) {
        state->exit_code = code;
        state->has_exit_code = true;
        if (code && (code >= sizeof state->unmet || !state->unmet[code])) error = "check-failed";
    }
    if (check.capture_stdout) {
        if (!error && fseek(check.capture_stdout, 0, SEEK_SET)) error = "invalid-output";
        if (!error) error = read_value(check.capture_stdout, &state->detected);
        if (fclose(check.capture_stdout) && !error) error = "cleanup-stage";
    }
    if (!error) {
        bool satisfied = !code && (!state->required || !strcmp(state->required, state->detected));
        state->status = satisfied ? BP_SATISFIED : BP_UNMET;
    }
    return error;
}

static const char *check_state(const struct bp_lock *lock, const char *workspace, struct bp_state *state,
                               struct runtime_output *outputs, bool preview, bool *started,
                               bool *repair_completed)
{
    const char *error = observe_state(lock, workspace, state, outputs, started);
    if (error || state->status == BP_SATISFIED) return error;
    if (!state->has_change) return "host-requirement-unmet";
    if (preview) return NULL;
    error = command(lock, workspace, &state->change, outputs, NULL, started);
    if (error) return error;
    error = observe_state(lock, workspace, state, outputs, started);
    if (!error && state->status == BP_UNMET) error = "recheck-unmet";
    if (error) {
        *repair_completed = state->required != NULL;
        return error;
    }
    state->status = BP_CHANGED;
    return NULL;
}

/* An earlier unpack can supply a parent directory that does not exist yet.
   Exact output collisions can already be diagnosed without creating it. */
static const char *archive_dependency(const char *workspace, const struct bp_plan *plan,
                                      size_t index, size_t *ready_after)
{
    const struct bp_step *step = &plan->steps[index];
    *ready_after = 0;
    if (step->op != BP_COPY && step->op != BP_REPLACE && step->op != BP_UNPACK) return NULL;
    if (!bp_safe_path(step->path)) return "unsafe-output-path";
    size_t length = strlen(step->path);
    for (size_t i = 0; i < index; ++i) {
        const struct bp_step *prior = &plan->steps[i];
        if (prior->op != BP_UNPACK &&
            !(step->op == BP_UNPACK && (prior->op == BP_COPY || prior->op == BP_REPLACE))) continue;
        size_t n = strlen(prior->path);
        if (n > length || (n < length && step->path[n] != '/') || !brw_same_ascii(prior->path, step->path, n))
            continue;
        if (strncmp(prior->path, step->path, n)) {
            char prefix[BP_PACKAGE_PATH_CAP];
            memcpy(prefix, step->path, n);
            prefix[n] = 0;
            enum bp_output_match match;
            const char *error = bp_compare_output_case(workspace, prior->path, prefix, &match);
            if (error) return error;
            if (match == BP_UNKNOWN_CASE) return "path-case-unknown";
            if (match == BP_DISTINCT_OUTPUTS) continue;
        }
        if (n == length || prior->op != BP_UNPACK) return "destination-exists";
        *ready_after = i + 1;
    }
    return NULL;
}

/* At the start of a segment, validate every edit whose parent is available.
   After an unpack, validate the edits that were waiting for that directory. */
static const char *validate_segment(const char *workspace, const struct bp_plan *plan,
                                    size_t start, size_t end, const size_t *ready_after,
                                    bool after_unpack, size_t *failed)
{
    bool recheck_files = start != 0;
    enum bp_edit_input inputs[BP_MAX_STEPS] = {BP_EDIT_SKIP};
    for (size_t i = start; i < end; ++i) {
        const struct bp_step *step = &plan->steps[i];
        if (step->state) recheck_files = true;
        if (after_unpack && ready_after[i] != start) continue;
        inputs[i] = ready_after[i] <= start ? BP_EDIT_WORKSPACE : BP_EDIT_FUTURE;
        if (inputs[i] == BP_EDIT_FUTURE || !recheck_files || (step->op != BP_COPY && step->op != BP_REPLACE)) continue;
        const char *error = bp_validate_step(workspace, step);
        if (error) {
            *failed = i;
            return error;
        }
    }
    return bp_validate_text_range(workspace, plan, start, end, inputs, failed);
}

const char *digby_run_blueprint(struct bp_plan *plan, const char *workspace, bool preview,
                                struct bp_run_result *result)
{
    *result = (struct bp_run_result){0};
    for (size_t i = 0; i < plan->count; ++i) {
        plan->steps[i].entry_status = BP_UNCHECKED;
        struct bp_state *state = plan->steps[i].state;
        if (state) {
            free(state->detected);
            state->detected = NULL;
            state->status = BP_UNCHECKED;
            state->has_exit_code = false;
        }
    }
    const char *error = validate_ssh_plan(plan, preview, result);
    if (error) return error;
    bool follows_state = false;
    size_t ready_after[BP_MAX_STEPS] = {0};
    for (size_t i = 0; i < plan->count; ++i) {
        const struct bp_step *s = &plan->steps[i];
        if (s->skip_op || s->op == BP_TOOL || s->op == BP_DEFAULT || s->op == BP_SSH) continue;
        error = archive_dependency(workspace, plan, i, &ready_after[i]);
        if (error) {
            result->failure_source = s->source;
            return error;
        }
        if (s->op == BP_UNPACK) {
            const struct digby_archive_plan archive = archive_plan(workspace, s);
            error = ready_after[i] ? digby_archive_validate(&archive) : digby_archive_extract(&archive, true);
        } else if (s->state) {
            follows_state = true;
            error = validate_step(workspace, &s->state->check);
            if (!error && s->state->has_change) error = validate_step(workspace, &s->state->change);
        } else if ((follows_state || ready_after[i]) && (s->op == BP_COPY || s->op == BP_REPLACE)) {
            error = bp_validate_file_operands(s);
        } else error = validate_step(workspace, s);
        if (error) {
            result->failure_source = s->source;
            return error;
        }
    }
    size_t segment_end = 0;
    bool earlier_effect = false, earlier_entry_effect = false;
    struct runtime_output outputs[BP_MAX_STEPS] = {0};
    size_t check = 0;
    for (size_t i = 0; !error && i <= plan->count; ++i) {
        while (!preview && check < plan->check_count) {
            size_t j = plan->checks[check];
            if (plan->outputs[j].end != i) break;
            ++check;
            error = read_value(outputs[j].file.stream, &outputs[j].value);
            if (outputs[j].file.path) {
                if (!bp_remove_output_file(&outputs[j].file) && !error) error = "cleanup-stage";
                outputs[j].file = (struct bp_output_file){0};
            }
            if (error) {
                result->failure_source = plan->outputs[j].source;
                break;
            }
        }
        if (error || i == plan->count) break;
        struct bp_step *step = &plan->steps[i];
        bool new_segment = i >= segment_end && !step->state;
        bool after_unpack = !preview && i && i < segment_end && plan->steps[i-1].op == BP_UNPACK;
        if (new_segment) {
            segment_end = i + 1;
            while (segment_end < plan->count) {
                const struct bp_state *state = plan->steps[segment_end].state;
                if (state && state->has_change) break;
                ++segment_end;
            }
        }
        if ((new_segment && (!preview || !earlier_effect)) || after_unpack) {
            size_t failed = i;
            error = validate_segment(workspace, plan, i, segment_end, ready_after, after_unpack, &failed);
            if (error) {
                result->failure_source = plan->steps[failed].source;
                break;
            }
        }
        if (step->skip_op) continue;
        bool started = false;
        if (step->op == BP_TOOL) {
            earlier_effect = true;
            if (preview) continue;
            error = bp_apply_packages(plan->lock, i);
        } else if (step->op == BP_DEFAULT) {
            bool matches = false, changed = false;
            if (preview && earlier_entry_effect) {
                step->entry_status = BP_DEFERRED;
            } else if (preview) {
                error = bp_check_default(plan->lock, step, &matches);
                if (!error) step->entry_status = matches ? BP_SATISFIED : BP_UNMET;
            } else {
                error = bp_use_default(plan->lock, step, &changed);
                if (!error) step->entry_status = changed ? BP_CHANGED : BP_SATISFIED;
            }
            if (step->entry_status != BP_SATISFIED) earlier_effect = earlier_entry_effect = true;
        } else if (step->state) {
            if (preview && (earlier_effect || needs_output(&step->state->check))) {
                earlier_effect = earlier_entry_effect = true;
                step->state->status = BP_DEFERRED;
                continue;
            }
            error = check_state(plan->lock, workspace, step->state, outputs, preview, &started,
                                &result->repair_completed);
            if (!error && step->state->status != BP_SATISFIED) earlier_effect = earlier_entry_effect = true;
        } else {
            earlier_effect = earlier_entry_effect = true;
            if (preview) continue;
            if (step->op == BP_SSH) {
                struct bp_ssh *ssh = step->ssh;
                ssh->result = run_ssh(plan, ssh, false);
                ssh->reported = true;
                started = ssh->result.status == DIGBY_SSH_COMPLETED;
                result->remote_result_unknown = ssh->result.status == DIGBY_SSH_UNKNOWN;
                error = ssh->result.error;
                if (started && ssh->result.remote_exit) error = "child-failed";
            } else if (step->op == BP_UNPACK) {
                const struct digby_archive_plan archive = archive_plan(workspace, step);
                error = digby_archive_extract(&archive, false);
            } else if (step->op == BP_RUN) {
                error = command(plan->lock, workspace, step, outputs, NULL, &started);
            } else error = bp_apply_step(workspace, step);
        }
        if (error) {
            result->failure_source = step->source;
            result->command_started = started;
        } else if (!preview) ++result->completed;
    }
    for (size_t j = 0; j < plan->output_count; ++j) {
        if (outputs[j].file.path && !bp_remove_output_file(&outputs[j].file) && !error) {
            error = "cleanup-stage";
            result->failure_source = plan->outputs[j].source;
        }
        free(outputs[j].value);
    }
    return error;
}
