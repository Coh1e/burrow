#include "blueprint.h"
#include "digby_archive.h"
#include <quickjs.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { JS_BYTES = 16 * 1024 * 1024, MAX_DEPTH = 24, MAX_FIELD = 4096 };

struct planner {
    const struct bp_bundle *bundle;
    struct bp_plan *plan;
    char *detail;
    const char *error;
    const char *call;
    const char *producer;
    const struct bp_file *active[MAX_DEPTH];
    JSValue compiled[BP_MAX_FILES];
    JSValue snapshot, input_scope;
    size_t visits;
    unsigned interrupts;
    clock_t start;
};

static void fail(struct planner *p, const char *error, const char *detail)
{
    if (!p->error) {
        p->error = error;
        snprintf(p->detail, BP_DETAIL_CAP, "%s", detail);
    }
}

static int interrupt(JSRuntime *rt, void *opaque)
{
    struct planner *p = opaque;
    (void)rt;
    if (++p->interrupts < 5000 &&
        ((double)clock() - (double)p->start) / CLOCKS_PER_SEC < 2.0) return 0;
    char detail[BP_DETAIL_CAP];
    snprintf(detail, sizeof detail, "%s: execution budget exhausted", p->call);
    fail(p, "blueprint-limit", detail);
    return 1;
}

/* Resolve from the declaring module, accepting parent prefixes only while
   there is still a directory component inside the verified bundle. */
static bool relative_path(const char *base, const char *ref, char path[BP_DETAIL_CAP])
{
    const char *slash = strrchr(base, '/');
    size_t prefix = slash ? (size_t)(slash - base + 1) : 0;
    if (!strncmp(ref, "./", 2)) ref += 2;
    while (!strncmp(ref, "../", 3)) {
        if (!prefix) return false;
        --prefix;
        while (prefix && base[prefix - 1] != '/') --prefix;
        ref += 3;
    }
    if (!bp_safe_path(ref) || prefix + strlen(ref) >= BP_DETAIL_CAP) return false;
    memcpy(path, base, prefix);
    strcpy(path + prefix, ref);
    return true;
}

static char *normalize(JSContext *ctx, const char *base, const char *ref, void *opaque)
{
    struct planner *p = opaque;
    char path[BP_DETAIL_CAP];
    const char *error = NULL;
    if ((strncmp(ref, "./", 2) && strncmp(ref, "../", 3)) || !relative_path(base, ref, path))
        error = "invalid-import";
    else {
        const struct bp_file *member = bp_find(p->bundle, path);
        if (!member || !member->module) error = "missing-module";
    }
    if (error) {
        char detail[BP_DETAIL_CAP];
        snprintf(detail, sizeof detail, "%s (%s imports %s)", p->call, base, ref);
        fail(p, error, detail);
        JS_ThrowReferenceError(ctx, "invalid package import");
        return NULL;
    }
    return js_strdup(ctx, path);
}

static JSValue compile(JSContext *ctx, struct planner *p, const struct bp_file *file)
{
    size_t i = (size_t)(file - p->bundle->files);
    if (JS_IsUndefined(p->compiled[i])) {
        JSValue module = JS_Eval(ctx, (const char *)file->data, file->size, file->path,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(module)) return module;
        p->compiled[i] = module;
    }
    return JS_DupValue(ctx, p->compiled[i]);
}

static JSModuleDef *load_module(JSContext *ctx, const char *name, void *opaque)
{
    struct planner *p = opaque;
    const struct bp_file *file = bp_find(p->bundle, name);
    if (!file || !file->module) {
        fail(p, "missing-module", name);
        JS_ThrowReferenceError(ctx, "missing verified module");
        return NULL;
    }
    JSValue module = compile(ctx, p, file);
    if (JS_IsException(module)) return NULL;
    JSModuleDef *def = JS_VALUE_GET_PTR(module);
    JS_FreeValue(ctx, module);
    return def;
}

/* Return owned C text. Embedded NULs must never truncate paths or arguments. */
static char *copy_string(JSContext *ctx, JSValueConst value)
{
    size_t length;
    if (!JS_IsString(value)) return NULL;
    const char *text = JS_ToCStringLen(ctx, &length, value);
    char *copy = NULL;
    if (text && length <= MAX_FIELD && !memchr(text, '\0', length)) {
        copy = malloc(length + 1);
        if (copy) memcpy(copy, text, length + 1);
    }
    if (text) JS_FreeCString(ctx, text);
    return copy;
}

static char *field(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    char *copy = copy_string(ctx, value);
    JS_FreeValue(ctx, value);
    return copy;
}

static bool unsigned_integer(JSContext *ctx, JSValueConst value, size_t *out)
{
    double n;
    bool ok = JS_IsNumber(value) && !JS_ToFloat64(ctx, &n, value) &&
              n >= 0 && n <= UINT32_MAX && (double)(uint32_t)n == n;
    if (ok) *out = (size_t)n;
    return ok;
}

static bool integer_field(JSContext *ctx, JSValueConst obj, const char *key, size_t *out)
{
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    bool ok = unsigned_integer(ctx, value, out);
    JS_FreeValue(ctx, value);
    return ok;
}

/* Use only a data property from the engine's Error object. Reporting a
   rejected Blueprint must not invoke a user-defined message getter. */
static void evaluation_error(JSContext *ctx, struct planner *p, JSValue exception,
                             const char *location)
{
    const char *code = "module-evaluation";
    char detail[BP_DETAIL_CAP];
    snprintf(detail, sizeof detail, "%s", location);
    if (JS_IsError(ctx, exception)) {
        JSAtom atom = JS_NewAtom(ctx, "message");
        JSPropertyDescriptor desc;
        if (atom && JS_GetOwnProperty(ctx, &desc, exception, atom) == 1) {
            char *message = copy_string(ctx, desc.value);
            if (message) {
                if (strstr(message, "out of memory") || strstr(message, "stack overflow"))
                    code = "blueprint-limit";
                snprintf(detail, sizeof detail, "%s: %s", location, message);
                free(message);
            }
            JS_FreeValue(ctx, desc.value);
            JS_FreeValue(ctx, desc.getter);
            JS_FreeValue(ctx, desc.setter);
        }
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, exception);
    fail(p, code, detail);
}

static void free_command(struct bp_step *s)
{
    for (size_t j = 0; j < s->argc; ++j) free(s->args[j]);
    free(s->bindings);
    free(s->args);
    free(s->program);
    free(s->tool);
}

static void free_pack(struct bp_pack *pack)
{
    if (!pack) return;
    for (size_t i = 0; i < pack->count; ++i) free(pack->inputs[i]);
    free(pack->format);
    free(pack);
}

void bp_plan_free(struct bp_plan *plan)
{
    if (!plan) return;
    for (size_t i = 0; i < plan->count; ++i) {
        struct bp_step *s = &plan->steps[i];
        free_command(s);
        if (s->state) {
            free_command(&s->state->check);
            free_command(&s->state->change);
            free(s->state->required);
            free(s->state->detected);
            free(s->state);
        }
        if (s->ssh) {
            free(s->ssh->target);
            free(s->ssh->command);
            free(s->ssh);
        }
        free_pack(s->pack);
        free(s->url);
        free(s->sha256);
        free(s->skip_platform);
        free(s->skip_op);
        free(s->registry);
        free(s->source); free(s->path); free(s->from); free(s->to);
        free(s->name); free(s->version); free(s->entry); free(s->value);
    }
    for (size_t i = 0; i < plan->output_count; ++i) {
        free(plan->outputs[i].name);
        free(plan->outputs[i].source);
    }
    plan->check_count = plan->output_count = plan->count = 0;
}

static const struct bp_file *payload_field(JSContext *ctx, JSValueConst obj,
                                          const struct bp_bundle *bundle, const char *module)
{
    char *ref = field(ctx, obj, "payload"), path[BP_DETAIL_CAP];
    const struct bp_file *file = ref && relative_path(module, ref, path) ? bp_find(bundle, path) : NULL;
    free(ref);
    return file && !file->module ? file : NULL;
}

static char *duplicate(const char *value)
{
    char *result = malloc(strlen(value) + 1);
    if (result) strcpy(result, value);
    return result;
}

static void declare_outputs(JSContext *ctx, struct planner *p, JSValueConst ns)
{
    JSValue list = JS_GetPropertyStr(ctx, ns, "outputs");
    size_t count = 0;
    if (!JS_IsUndefined(list) && (JS_IsArray(ctx, list) != 1 ||
        !integer_field(ctx, list, "length", &count) ||
        count > BP_MAX_STEPS - p->plan->output_count)) fail(p, "invalid-outputs", p->call);
    size_t start = p->plan->output_count;
    for (size_t i = 0; i < count && !p->error; ++i) {
        JSValue key = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
        struct bp_output *output = &p->plan->outputs[p->plan->output_count++];
        output->name = copy_string(ctx, key);
        JS_FreeValue(ctx, key);
        output->source = duplicate(p->producer);
        output->end = SIZE_MAX;
        if (!output->source || !output->name || !*output->name)
            fail(p, "invalid-outputs", p->call);
        for (size_t j = start; !p->error && j + 1 < p->plan->output_count; ++j)
            if (!strcmp(p->plan->outputs[j].name, output->name)) fail(p, "invalid-outputs", p->call);
    }
    JS_FreeValue(ctx, list);
}

static bool reference_source(JSContext *ctx, JSValueConst ref, const char *scope,
                             char source[BP_DETAIL_CAP])
{
    size_t index;
    return integer_field(ctx, ref, "from", &index) && index &&
           snprintf(source, BP_DETAIL_CAP, "%s[%zu]", scope, index) < BP_DETAIL_CAP;
}

static void bind_output(struct planner *p, struct bp_binding *binding, const char *name,
                        const char *source, bool writer, const char *consumer)
{
    for (size_t j = 0; j < p->plan->output_count; ++j) {
        struct bp_output *o = &p->plan->outputs[j];
        if ((writer || o->end != SIZE_MAX) && !strcmp(source, o->source) && !strcmp(name, o->name)) {
            binding->output = j + 1;
            binding->writer = writer;
            return;
        }
    }
    char detail[BP_DETAIL_CAP];
    if (snprintf(detail, sizeof detail, "%s: %s (%s)", consumer, source, name) >= (int)sizeof detail) {
        fail(p, "blueprint-limit", consumer);
        return;
    }
    fail(p, "unavailable-output", detail);
}

static const char *parse_command(JSContext *ctx, JSValueConst obj, struct planner *p,
                                 const char *module, struct bp_step *s)
{
    JSValue program = JS_GetPropertyStr(ctx, obj, "program");
    JSValue tool = JS_GetPropertyStr(ctx, obj, "tool");
    s->program = copy_string(ctx, program);
    s->tool = copy_string(ctx, tool);
    bool valid_program = JS_IsUndefined(tool) && s->program && *s->program && !strchr(s->program, '"');
    bool valid_tool = JS_IsUndefined(program) && s->tool && *s->tool;
    JS_FreeValue(ctx, tool);
    JS_FreeValue(ctx, program);
    JSValue args = JS_GetPropertyStr(ctx, obj, "args");
    const char *error = "invalid-command";
    size_t length;
    if ((!valid_program && !valid_tool) || JS_IsArray(ctx, args) != 1 ||
        !integer_field(ctx, args, "length", &length) || length > BP_MAX_ARGS) goto done;
    s->args = calloc(length + 1, sizeof *s->args);
    s->bindings = calloc(length + 1, sizeof *s->bindings);
    if (!s->args || !s->bindings) { error = "out-of-memory"; goto done; }
    for (size_t i = 0; i < length; ++i) {
        JSValue arg = JS_GetPropertyUint32(ctx, args, (uint32_t)i);
        char *write_name = JS_IsObject(arg) ? field(ctx, arg, "outputFile") : NULL;
        char *read_name = JS_IsObject(arg) ? field(ctx, arg, "output") : NULL;
        if (write_name || read_name) {
            char position[BP_DETAIL_CAP];
            JSValue origin = read_name ? JS_Call(ctx, p->input_scope, JS_UNDEFINED, 1, &arg) : JS_UNDEFINED;
            char *bound = copy_string(ctx, origin);
            JS_FreeValue(ctx, origin);
            const char *source = write_name ? p->producer : NULL;
            if (!source && reference_source(ctx, arg, bound ? bound : p->call, position)) source = position;
            if (!source || (write_name && read_name)) fail(p, "invalid-output-reference", s->source);
            else bind_output(p, &s->bindings[i], write_name ? write_name : read_name,
                             source, write_name != NULL, s->source);
            free(bound);
            free(write_name);
            free(read_name);
            JS_FreeValue(ctx, arg);
            if (p->error) goto done;
            s->args[i] = duplicate(""); /* Read-only validation placeholder. */
            if (!s->args[i]) { error = "out-of-memory"; goto done; }
        } else if (JS_IsObject(arg) && !s->payload) {
            s->payload = payload_field(ctx, arg, p->bundle, module);
            JS_FreeValue(ctx, arg);
            if (!s->payload) { error = "invalid-payload"; goto done; }
            s->script_arg = i;
        } else {
            s->args[i] = copy_string(ctx, arg);
            JS_FreeValue(ctx, arg);
            if (!s->args[i]) goto done;
        }
        ++s->argc;
    }
    error = NULL;
done:
    JS_FreeValue(ctx, args);
    return error;
}

static bool parse_unmet_codes(JSContext *ctx, JSValueConst obj, struct bp_state *state)
{
    JSValue list = JS_GetPropertyStr(ctx, obj, "unmet");
    if (JS_IsUndefined(list)) return state->required != NULL;
    size_t length;
    bool valid = JS_IsArray(ctx, list) == 1 && integer_field(ctx, list, "length", &length) &&
                 length <= 255;
    for (size_t i = 0; valid && i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
        size_t code;
        valid = unsigned_integer(ctx, item, &code) && code && code <= 255 && !state->unmet[code];
        JS_FreeValue(ctx, item);
        if (valid) state->unmet[code] = 1;
    }
    JS_FreeValue(ctx, list);
    return valid;
}

static const char *parse_state(JSContext *ctx, JSValueConst obj, struct planner *p,
                               const char *module, struct bp_step *s)
{
    struct bp_state *state = s->state = calloc(1, sizeof *state);
    if (!state) return "out-of-memory";
    bool host = s->op == BP_HOST;
    if (host) {
        state->required = field(ctx, obj, "required");
        if (!state->required || !*state->required || strpbrk(state->required, "\r\n") ||
            !bp_valid_utf8(state->required, strlen(state->required)))
            return "invalid-host-requirement";
    }
    if (!parse_unmet_codes(ctx, obj, state)) return "invalid-unmet-codes";
    for (unsigned i = 0; i < 2; ++i) {
        struct bp_step *command = i ? &state->change : &state->check;
        command->op = BP_RUN;
        command->source = s->source;
        const char *key = i ? (host ? "repair" : "change") : "check";
        JSValue value = JS_GetPropertyStr(ctx, obj, key);
        if (host && i && JS_IsUndefined(value)) return NULL;
        const char *error = JS_IsObject(value) ? parse_command(ctx, value, p, module, command)
                                               : "invalid-command";
        JS_FreeValue(ctx, value);
        if (error) return error;
        for (size_t j = 0; j < command->argc; ++j)
            if (command->bindings[j].writer) return "invalid-output-reference";
        if (i) state->has_change = true;
    }
    return NULL;
}

/* SSH operands describe the requested operation. Authorization and trust
   files enter through the CLI, so reject other operands at the step location. */
static bool ssh_fields(JSContext *ctx, JSValueConst obj)
{
    static const char *const allowed[] = {"op", "platform", "target", "command", "port", "timeoutSeconds"};
    JSPropertyEnum *keys;
    uint32_t count;
    if (JS_GetOwnPropertyNames(ctx, &keys, &count, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return false;
    bool valid = true;
    for (uint32_t i = 0; valid && i < count; ++i) {
        JSValue name = JS_AtomToValue(ctx, keys[i].atom);
        char *key = copy_string(ctx, name);
        JS_FreeValue(ctx, name);
        valid = false;
        for (size_t j = 0; key && j < sizeof allowed / sizeof *allowed; ++j) {
            if (!strcmp(key, allowed[j])) { valid = true; break; }
        }
        free(key);
    }
    JS_FreePropertyEnum(ctx, keys, count);
    return valid;
}

static bool ssh_number(JSContext *ctx, JSValueConst obj, const char *key, unsigned maximum,
                       unsigned fallback, unsigned *out)
{
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    size_t number = fallback;
    bool valid = JS_IsUndefined(value) ||
        (unsigned_integer(ctx, value, &number) && number > 0 && number <= maximum);
    JS_FreeValue(ctx, value);
    if (valid) *out = (unsigned)number;
    return valid;
}

static const char *parse_ssh(JSContext *ctx, JSValueConst obj, struct bp_step *step)
{
    if (!ssh_fields(ctx, obj)) return "invalid-ssh-request";
    struct bp_ssh *ssh = step->ssh = calloc(1, sizeof *ssh);
    if (!ssh) return "out-of-memory";
    ssh->target = field(ctx, obj, "target");
    ssh->command = field(ctx, obj, "command");
    if (!ssh->target || !*ssh->target || !bp_valid_utf8(ssh->target, strlen(ssh->target)) ||
        !ssh->command || !*ssh->command ||
        !bp_valid_utf8(ssh->command, strlen(ssh->command)) ||
        !ssh_number(ctx, obj, "port", 65535, 22, &ssh->port) ||
        !ssh_number(ctx, obj, "timeoutSeconds", UINT_MAX, 30, &ssh->timeout_seconds))
        return "invalid-ssh-request";
    return NULL;
}

static const char *parse_pack(JSContext *ctx, JSValueConst obj, struct bp_step *step)
{
    JSValue inputs = JS_GetPropertyStr(ctx, obj, "inputs");
    struct bp_pack *pack = NULL;
    size_t count;
    const char *error = "invalid-archive-plan";
    if (JS_IsArray(ctx, inputs) != 1 || !integer_field(ctx, inputs, "length", &count) || !count) goto done;
    if (count > DIGBY_ARCHIVE_MEMBER_LIMIT) { error = "archive-limit"; goto done; }
    pack = calloc(1, sizeof *pack + count * sizeof *pack->inputs);
    if (!pack) { error = "out-of-memory"; goto done; }
    pack->count = count;
    pack->format = field(ctx, obj, "format");
    if (!pack->format || !bp_valid_utf8(pack->format, strlen(pack->format))) goto done;
    for (size_t i = 0; i < count; ++i) {
        JSValue input = JS_GetPropertyUint32(ctx, inputs, (uint32_t)i);
        pack->inputs[i] = copy_string(ctx, input);
        JS_FreeValue(ctx, input);
        if (!bp_safe_path(pack->inputs[i])) { error = "archive-path"; goto done; }
    }
    step->pack = pack;
    error = NULL;
done:
    if (error) free_pack(pack);
    JS_FreeValue(ctx, inputs);
    return error;
}

static void add_step(JSContext *ctx, struct planner *p, JSValueConst obj, const char *op,
                     const struct bp_file *module, const char *location)
{
    struct bp_step *s = &p->plan->steps[p->plan->count++];
    memset(s, 0, sizeof *s);
    s->source = malloc(strlen(location) + 1);
    if (!s->source) { fail(p, "blueprint-limit", location); return; }
    strcpy(s->source, location);
    for (s->op = BP_COPY; s->op < BP_OP_COUNT; ++s->op)
        if (!strcmp(op, bp_op_name(s->op))) break;
    if (s->op == BP_OP_COUNT) { fail(p, "invalid-step", location); return; }
    if (p->bundle->unavailable & (1u << s->op)) {
        char detail[BP_DETAIL_CAP];
        if (snprintf(detail, sizeof detail, "%s: %s", location, op) >= (int)sizeof detail)
            fail(p, "blueprint-limit", location);
        else fail(p, "missing-capability", detail);
        return;
    }
    if (!(p->bundle->required & (1u << s->op))) {
        fail(p, "undeclared-capability", location);
        return;
    }
    if (s->op == BP_COPY || s->op == BP_INSTALL || s->op == BP_UNPACK) {
        s->payload = payload_field(ctx, obj, p->bundle, module->path);
        if (!s->payload) { fail(p, "invalid-payload", location); return; }
    }
    if (s->op == BP_COPY || s->op == BP_REPLACE || s->op == BP_UNPACK || s->op == BP_PACK) {
        s->path = field(ctx, obj, "path");
        if (!s->path || !bp_safe_path(s->path)) { fail(p, "invalid-path", location); return; }
    }
    switch (s->op) {
    case BP_DOWNLOAD: {
        s->path = field(ctx, obj, "path");
        char *url = field(ctx, obj, "url");
        s->sha256 = field(ctx, obj, "sha256");
        if (!s->path || !s->sha256 || !url) fail(p, "invalid-step", location);
        else if (!brw_valid_sha256(s->sha256)) fail(p, "invalid-digest", location);
        else if (!brw_valid_http_url(url)) fail(p, "download-url", location);
        else {
            s->url = url;
            break;
        }
        free(url);
        break;
    }
    case BP_PACK: {
        const char *error = parse_pack(ctx, obj, s);
        if (error) fail(p, error, location);
        break;
    }
    case BP_SSH: {
        const char *error = parse_ssh(ctx, obj, s);
        if (error) fail(p, error, location);
        break;
    }
    case BP_DEFAULT:
        s->tool = field(ctx, obj, "tool");
        if (!s->tool || !*s->tool) fail(p, "invalid-tool", location);
        break;
    case BP_TOOL: {
        JSValue registry = JS_GetPropertyStr(ctx, obj, "registry");
        s->registry = copy_string(ctx, registry);
        bool valid_registry = JS_IsUndefined(registry) || (s->registry && *s->registry);
        JS_FreeValue(ctx, registry);
        s->name = field(ctx, obj, "name");
        s->version = field(ctx, obj, "version");
        if (!s->name || !*s->name || !s->version || !*s->version ||
            !valid_registry) fail(p, "invalid-tool", location);
        break;
    }
    case BP_STATE:
    case BP_HOST: {
        const char *error = parse_state(ctx, obj, p, module->path, s);
        if (error) fail(p, error, location);
        break;
    }
    case BP_RUN: {
        const char *error = parse_command(ctx, obj, p, module->path, s);
        if (error) fail(p, error, location);
        break;
    }
    case BP_COPY: break;
    case BP_REPLACE:
        s->from = field(ctx, obj, "from");
        s->to = field(ctx, obj, "to");
        if (!s->from || !*s->from || !s->to || !integer_field(ctx, obj, "count", &s->count))
            fail(p, "invalid-step", location);
        break;
    case BP_INSTALL:
        s->name = field(ctx, obj, "name");
        s->version = field(ctx, obj, "version");
        s->entry = field(ctx, obj, "entry");
        if (!s->name || !s->version || !s->entry) fail(p, "invalid-step", location);
        break;
    case BP_ENV:
        s->name = field(ctx, obj, "name");
        s->value = field(ctx, obj, "value");
        if (!s->name || !s->value) fail(p, "invalid-step", location);
        break;
    default: break;
    }
}

static void expand(JSContext *ctx, struct planner *p, const struct bp_file *file,
                   JSValueConst args, const char *location, size_t depth);

/* Carry the lexical scope alongside each "from" field across a JSON snapshot.
   Restore the original field and keep its origin private to the planner. */
static const char input_helpers[] =
    "(() => {"
    "  const scopes = new WeakMap(), get = scopes.get.bind(scopes), set = scopes.set.bind(scopes);"
    "  const {parse, stringify} = JSON;"
    "  function snapshot(args, caller) {"
    "    const json = stringify(args, function(key, value) {"
    "      if (key === 'from' && value !== undefined && typeof value !== 'function' && typeof value !== 'symbol')"
    "        return [value, get(this) || caller];"
    "      return value;"
    "    });"
    "    return parse(json, function(key, value) {"
    "      if (key === 'from') { set(this, value[1]); return value[0]; }"
    "      return value;"
    "    });"
    "  }"
    "  return [snapshot, get];"
    "})()";

static void include(JSContext *ctx, struct planner *p, JSValueConst obj,
                    const struct bp_file *parent, const char *position, size_t depth)
{
    char *ref = field(ctx, obj, "module");
    char path[BP_DETAIL_CAP], call[BP_DETAIL_CAP];
    JSValue args = JS_GetPropertyStr(ctx, obj, "args");
    if (!ref || !relative_path(parent->path, ref, path)) fail(p, "invalid-import", position);
    else if (!JS_IsObject(args) || JS_IsArray(ctx, args) || JS_IsFunction(ctx, args))
        fail(p, "invalid-arguments", position);
    else {
        const struct bp_file *file = bp_find(p->bundle, path);
        if (snprintf(call, sizeof call, "%s > %s", position, path) >= (int)sizeof call)
            fail(p, "blueprint-limit", position);
        else if (!file || !file->module) fail(p, "missing-module", call);
        else {
            JSValue inputs[] = {args, JS_NewString(ctx, p->call)};
            JSValue bound = JS_Call(ctx, p->snapshot, JS_UNDEFINED, 2, inputs);
            JS_FreeValue(ctx, inputs[1]);
            if (!JS_IsObject(bound) || JS_IsArray(ctx, bound))
                fail(p, "invalid-arguments", position);
            else {
                const char *caller = p->call;
                const char *producer = p->producer;
                p->producer = position;
                p->call = call;
                expand(ctx, p, file, bound, call, depth + 1);
                p->call = caller;
                p->producer = producer;
            }
            JS_FreeValue(ctx, bound);
        }
    }
    JS_FreeValue(ctx, args);
    free(ref);
}

static bool platform_skips(JSContext *ctx, struct planner *p, JSValueConst obj,
                           const char *position, const char *op)
{
    JSValue value = JS_GetPropertyStr(ctx, obj, "platform");
    bool specified = !JS_IsUndefined(value);
    char *platform = copy_string(ctx, value);
    JS_FreeValue(ctx, value);
    if (specified && (!platform || (strcmp(platform, "any") && strcmp(platform, "windows") &&
        strcmp(platform, "linux") && strcmp(platform, "macos")))) fail(p, "invalid-platform", position);
    if (p->error || !platform || !strcmp(platform, "any") || !strcmp(platform, p->bundle->platform)) {
        free(platform);
        return p->error != NULL;
    }
    struct bp_step *s = &p->plan->steps[p->plan->count++];
    *s = (struct bp_step){.op = BP_OP_COUNT};
    s->skip_platform = platform;
    s->skip_op = duplicate(op);
    s->source = duplicate(position);
    if (!s->source || !s->skip_op) fail(p, "out-of-memory", position);
    return true;
}

static void verify_inputs(JSContext *ctx, struct planner *p, JSValueConst ns, JSValueConst args,
                          const char *location)
{
    JSValue inputs = JS_GetPropertyStr(ctx, ns, "inputs");
    size_t count = 0;
    if (!JS_IsUndefined(inputs) && (JS_IsArray(ctx, inputs) != 1 ||
        !integer_field(ctx, inputs, "length", &count) || count > BP_MAX_STEPS)) {
        fail(p, "invalid-inputs", location);
    }
    for (size_t i = 0; !p->error && i < count; ++i) {
        JSValue key = JS_GetPropertyUint32(ctx, inputs, (uint32_t)i);
        char *name = copy_string(ctx, key);
        if (!name || !*name) fail(p, "invalid-inputs", location);
        else {
            JSAtom atom = JS_NewAtom(ctx, name);
            if (!atom || JS_GetOwnProperty(ctx, NULL, args, atom) != 1) {
                char detail[BP_DETAIL_CAP];
                snprintf(detail, sizeof detail, "%s: %s", location, name);
                fail(p, "missing-input", detail);
            }
            JS_FreeAtom(ctx, atom);
        }
        free(name);
        JS_FreeValue(ctx, key);
    }
    JS_FreeValue(ctx, inputs);
}

static void expand(JSContext *ctx, struct planner *p, const struct bp_file *file,
                   JSValueConst args, const char *location, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        if (p->active[i] == file) { fail(p, "blueprint-cycle", location); return; }
    if (depth == MAX_DEPTH) { fail(p, "blueprint-limit", location); return; }
    p->active[depth] = file;
    JSValue module = compile(ctx, p, file);
    if (JS_IsException(module)) {
        evaluation_error(ctx, p, JS_GetException(ctx), location);
        return;
    }
    JSModuleDef *def = JS_VALUE_GET_PTR(module);
    JSValue job = JS_EvalFunction(ctx, module);
    if (JS_IsException(job)) evaluation_error(ctx, p, JS_GetException(ctx), location);
    else if (JS_PromiseState(ctx, job) == JS_PROMISE_REJECTED)
        evaluation_error(ctx, p, JS_PromiseResult(ctx, job), location);
    else if (JS_PromiseState(ctx, job) != JS_PROMISE_FULFILLED)
        fail(p, "async-blueprint", location);
    JS_FreeValue(ctx, job);
    if (p->error) return;
    JSValue ns = JS_GetModuleNamespace(ctx, def);
    size_t first_output = p->plan->output_count;
    declare_outputs(ctx, p, ns);
    size_t end_output = p->plan->output_count;
    verify_inputs(ctx, p, ns, args, location);
    JSValue fn = JS_GetPropertyStr(ctx, ns, "default");
    JSValue steps = p->error ? JS_UNDEFINED : JS_Call(ctx, fn, JS_UNDEFINED, 1, &args);
    if (JS_IsException(steps)) evaluation_error(ctx, p, JS_GetException(ctx), location);
    size_t n = 0;
    if (JS_IsArray(ctx, steps) != 1 || !integer_field(ctx, steps, "length", &n))
        fail(p, "invalid-blueprint", location);
    if (n > BP_MAX_STEPS - p->visits) fail(p, "blueprint-limit", location);
    else p->visits += n;
    for (size_t i = 0; i < n && !p->error; ++i) {
        char position[BP_DETAIL_CAP];
        if (snprintf(position, sizeof position, "%s[%zu]", location, i + 1) >= (int)sizeof position) {
            fail(p, "blueprint-limit", location);
            break;
        }
        JSValue obj = JS_GetPropertyUint32(ctx, steps, (uint32_t)i);
        char *op = field(ctx, obj, "op");
        if (!op) fail(p, "invalid-step", position);
        else if (!platform_skips(ctx, p, obj, position, op)) {
            if (!strcmp(op, "include")) include(ctx, p, obj, file, position, depth);
            else add_step(ctx, p, obj, op, file, position);
        }
        free(op);
        JS_FreeValue(ctx, obj);
    }
    for (size_t j = first_output; j < end_output; ++j) {
        p->plan->outputs[j].end = p->plan->count;
        p->plan->checks[p->plan->check_count++] = j;
    }
    JS_FreeValue(ctx, steps);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ns);
}

const char *bp_build(const struct bp_bundle *bundle, struct bp_plan *out, char detail[BP_DETAIL_CAP])
{
    if (!bundle || !bundle->entry || bundle->count > BP_MAX_FILES || !out || out->count || !detail)
        return "invalid-blueprint";
    detail[0] = 0;
    const struct bp_file *entry = bp_find(bundle, bundle->entry);
    if (!entry || !entry->module) return "missing-module";
    struct planner p = { .bundle = bundle, .plan = out, .detail = detail,
                         .call = entry->path, .producer = entry->path, .start = clock() };
    for (size_t i = 0; i < bundle->count; ++i) p.compiled[i] = JS_UNDEFINED;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return "blueprint-limit";
    JS_SetMemoryLimit(rt, JS_BYTES);
    JS_SetMaxStackSize(rt, 256 * 1024);
    JS_SetCanBlock(rt, 0);
    JS_SetInterruptHandler(rt, interrupt, &p);
    JS_SetModuleLoaderFunc(rt, normalize, load_module, &p);
    JSContext *ctx = JS_NewContextRaw(rt);
    if (!ctx || JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicEval(ctx) ||
        JS_AddIntrinsicStringNormalize(ctx) || JS_AddIntrinsicRegExp(ctx) ||
        JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicPromise(ctx)) {
        fail(&p, "blueprint-limit", entry->path);
    } else {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue math = JS_GetPropertyStr(ctx, global, "Math");
        if (JS_SetPropertyStr(ctx, math, "random", JS_UNDEFINED) < 0)
            fail(&p, "module-evaluation", entry->path);
        JSValue helpers = JS_Eval(ctx, input_helpers, sizeof input_helpers - 1, "<inputs>", JS_EVAL_TYPE_GLOBAL);
        p.snapshot = JS_GetPropertyUint32(ctx, helpers, 0);
        p.input_scope = JS_GetPropertyUint32(ctx, helpers, 1);
        if (!JS_IsFunction(ctx, p.snapshot) || !JS_IsFunction(ctx, p.input_scope))
            fail(&p, "blueprint-limit", entry->path);
        JS_FreeValue(ctx, helpers);
        JSValue args = JS_NewObject(ctx);
        if (!p.error) expand(ctx, &p, entry, args, entry->path, 0);
        if (JS_IsJobPending(rt)) fail(&p, "async-blueprint", entry->path);
        JS_FreeValue(ctx, args);
        JS_FreeValue(ctx, p.input_scope);
        JS_FreeValue(ctx, p.snapshot);
        JS_FreeValue(ctx, math);
        JS_FreeValue(ctx, global);
    }
    if (ctx) {
        for (size_t i = 0; i < bundle->count; ++i) JS_FreeValue(ctx, p.compiled[i]);
        JS_FreeContext(ctx);
    }
    JS_FreeRuntime(rt);
    if (p.error) bp_plan_free(out);
    return p.error;
}
