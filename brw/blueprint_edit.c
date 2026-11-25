#include "blueprint_edit.h"
#include <stdlib.h>

static bool valid_utf8(const char *text, size_t size)
{
    const unsigned char *p = (const unsigned char *)text, *end = p + size;
    while (p < end) {
        unsigned lead = *p++;
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        unsigned n = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
        if ((size_t)(end - p) < n || (lead == 0xe0 && *p < 0xa0) ||
            (lead == 0xed && *p >= 0xa0) || (lead == 0xf0 && *p < 0x90) ||
            (lead == 0xf4 && *p >= 0x90)) return false;
        while (n--) if ((*p++ & 0xc0) != 0x80) return false;
    }
    return true;
}

const char *bp_validate_file_operands(const struct bp_step *s)
{
    if (!bp_safe_path(s->path)) return "unsafe-output-path";
    if (s->op == BP_COPY)
        return s->payload && !s->payload->module && s->payload->data ? NULL : "invalid-payload";
    if (s->op != BP_REPLACE || !s->from || !*s->from || !s->to) return "invalid-text-step";
    return valid_utf8(s->from, strlen(s->from)) && valid_utf8(s->to, strlen(s->to)) ?
           NULL : "invalid-utf8";
}

static const char *replacement_size(const struct bp_step *s, const char *text,
                                    size_t size, size_t *out_size)
{
    if (size > BP_TEXT_LIMIT) return "text-limit";
    if (memchr(text, 0, size)) return "binary-text";
    if (!valid_utf8(text, size)) return "invalid-utf8";
    size_t from = strlen(s->from), to = strlen(s->to), matches = 0;
    if (!from) return "invalid-text-step";
    for (const char *p = text; (p = strstr(p, s->from)) != NULL; p += from) ++matches;
    if (matches != s->count) return "text-match-count";
    if (to > from && matches > (BP_TEXT_LIMIT - size) / (to - from)) return "text-limit";
    *out_size = size - matches * from + matches * to;
    return NULL;
}

const char *bp_replace_text(FILE *stage, const struct bp_step *s, const char *text, size_t size)
{
    size_t result;
    const char *error = replacement_size(s, text, size, &result);
    if (error) return error;
    return brw_literal(stage, text, s->from, s->to, &s->count) ? "write-text" : NULL;
}

static unsigned fold_ascii(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static const char *compare_outputs(const char *workspace, const char *a, const char *b,
                                   enum bp_output_match *match)
{
    *match = BP_SAME_OUTPUT;
    if (!strcmp(a, b)) return NULL;
    *match = BP_DISTINCT_OUTPUTS;
    for (size_t i = 0; a[i] || b[i]; ++i)
        if (fold_ascii((unsigned char)a[i]) != fold_ascii((unsigned char)b[i])) return NULL;
    return bp_compare_output_case(workspace, a, b, match);
}

/* Build the validated result in memory to supply the next edit on this
   particular path. Retain at most one input and one output per target. */
static char *replace_memory(const struct bp_step *step, const char *input, size_t size)
{
    char *out = malloc(size + 1);
    if (!out) return NULL;
    char *p = out;
    size_t from = strlen(step->from), to = strlen(step->to);
    const char *match;
    while ((match = strstr(input, step->from)) != NULL) {
        size_t prefix = (size_t)(match - input);
        memcpy(p, input, prefix);
        memcpy(p + prefix, step->to, to);
        p += prefix + to;
        input = match + from;
    }
    strcpy(p, input);
    return out;
}

const char *bp_validate_text_plan(const char *workspace, const struct bp_plan *plan, size_t *failed_step)
{
    if (!workspace || !plan || plan->count > BP_MAX_STEPS || !failed_step) return "invalid-plan";
    for (size_t i = 0; i < plan->count; ++i) {
        const struct bp_step *step = &plan->steps[i];
        if (step->op == BP_COPY || step->op == BP_REPLACE) {
            const char *error = bp_validate_file_operands(step);
            if (error) { *failed_step = i; return error; }
        }
    }
    bool checked[BP_MAX_STEPS] = {false};
    const char *first_error = NULL;
    size_t stop = plan->count;
    for (size_t i = 0; i < stop; ++i) {
        const struct bp_step *first = &plan->steps[i];
        if (checked[i] || (first->op != BP_COPY && first->op != BP_REPLACE)) continue;
        char *owned = NULL;
        const char *input = NULL;
        size_t size = 0;
        for (size_t j = i; j < stop; ++j) {
            const struct bp_step *s = &plan->steps[j];
            if (s->op != BP_REPLACE && s->op != BP_COPY) continue;
            enum bp_output_match match;
            const char *error = compare_outputs(workspace, first->path, s->path, &match);
            if (!error && match == BP_UNKNOWN_CASE)
                error = "path-case-unknown";
            if (error) {
                first_error = error;
                stop = j;
                break;
            }
            if (match == BP_DISTINCT_OUTPUTS) continue;
            checked[j] = true;
            if (s->op == BP_COPY) {
                if (j != i) error = "destination-exists";
                else {
                    input = (const char *)s->payload->data;
                    size = s->payload->size;
                }
            } else {
                if (!input) {
                    error = bp_read_workspace_text(workspace, s->path, &owned, &size);
                    input = owned;
                }
                size_t next_size = 0;
                if (!error) error = replacement_size(s, input, size, &next_size);
                if (!error) {
                    char *next = replace_memory(s, input, next_size);
                    if (!next) error = "out-of-memory";
                    free(owned);
                    owned = next;
                    input = next;
                    size = next_size;
                }
            }
            if (error) {
                first_error = error;
                stop = j;
                break;
            }
        }
        free(owned);
    }
    *failed_step = stop;
    return first_error;
}
