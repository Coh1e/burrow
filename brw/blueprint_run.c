#include "blueprint_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct runtime_output {
    struct bp_output_file file;
    char *value;
};

static const char *read_output(struct runtime_output *output)
{
    if (!output->file.stream) return "missing-output";
    char bytes[4097];
    size_t n = fread(bytes, 1, sizeof bytes, output->file.stream);
    if (ferror(output->file.stream) || n == sizeof bytes || memchr(bytes, 0, n))
        return "invalid-output";
    if (n && bytes[n - 1] == '\n') {
        --n;
        if (n && bytes[n - 1] == '\r') --n;
    }
    if (!n) return "missing-output";
    if (memchr(bytes, '\n', n) || memchr(bytes, '\r', n) || !bp_valid_utf8(bytes, n))
        return "invalid-output";
    output->value = malloc(n + 1);
    if (!output->value) return "out-of-memory";
    memcpy(output->value, bytes, n);
    output->value[n] = 0;
    return NULL;
}

static const char *command(const char *workspace, const struct bp_step *step,
                           struct runtime_output *outputs, unsigned long *exit_code)
{
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
    return bp_run_command(workspace, &command, true, exit_code);
}

static bool needs_output(const struct bp_step *command)
{
    for (size_t i = 0; i < command->argc; ++i)
        if (command->bindings[i].output) return true;
    return false;
}

static const char *check_state(const char *workspace, struct bp_state *state,
                               struct runtime_output *outputs, bool preview)
{
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        unsigned long code = 0;
        const char *error = command(workspace, &state->check, outputs, &code);
        if (error) return error;
        state->exit_code = code;
        state->has_exit_code = true;
        if (!code) {
            state->status = attempt ? "changed" : "satisfied";
            return NULL;
        }
        if (code >= sizeof state->unmet || !state->unmet[code]) return "check-failed";
        state->status = "unmet";
        if (attempt) return "recheck-unmet";
        if (preview) return NULL;
        state->has_exit_code = false;
        error = command(workspace, &state->change, outputs, NULL);
        if (error) return error;
    }
    return NULL;
}

const char *digby_run_blueprint(const struct bp_plan *plan, const char *workspace, bool preview,
                                size_t *completed, const char **failure_source)
{
    *completed = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        const struct bp_step *s = &plan->steps[i];
        if (s->skip_op) continue;
        const char *error;
        if (s->state) {
            error = bp_validate_step(workspace, &s->state->check);
            if (!error) error = bp_validate_step(workspace, &s->state->change);
        } else error = bp_validate_step(workspace, s);
        if (error) {
            *failure_source = s->source;
            return error;
        }
    }
    size_t failed = 0;
    const char *error = bp_validate_text_plan(workspace, plan, &failed);
    if (error) *failure_source = plan->steps[failed].source;
    if (error) return error;
    bool earlier_effect = false;
    struct runtime_output outputs[BP_MAX_STEPS] = {0};
    size_t check = 0;
    for (size_t i = 0; !error && i <= plan->count; ++i) {
        while (!preview && check < plan->check_count) {
            size_t j = plan->checks[check];
            if (plan->outputs[j].end != i) break;
            ++check;
            error = read_output(&outputs[j]);
            if (outputs[j].file.path) {
                if (!bp_remove_output_file(&outputs[j].file) && !error) error = "cleanup-stage";
                outputs[j].file = (struct bp_output_file){0};
            }
            if (error) {
                *failure_source = plan->outputs[j].source;
                break;
            }
        }
        if (error || i == plan->count) break;
        const struct bp_step *step = &plan->steps[i];
        if (step->skip_op) continue;
        if (step->state) {
            step->state->has_exit_code = false;
            step->state->status = NULL;
            if (preview && (earlier_effect || needs_output(&step->state->check))) {
                step->state->status = "check-at-execution";
                continue;
            }
            error = check_state(workspace, step->state, outputs, preview);
            if (!error && strcmp(step->state->status, "satisfied")) earlier_effect = true;
        } else {
            earlier_effect = true;
            if (preview) continue;
            error = step->op == BP_RUN ? command(workspace, step, outputs, NULL)
                                       : bp_apply_step(workspace, step);
        }
        if (error) *failure_source = step->source;
        else if (!preview) ++*completed;
    }
    for (size_t j = 0; j < plan->output_count; ++j) {
        if (outputs[j].file.path && !bp_remove_output_file(&outputs[j].file) && !error) {
            error = "cleanup-stage";
            *failure_source = plan->outputs[j].source;
        }
        free(outputs[j].value);
    }
    return error;
}
