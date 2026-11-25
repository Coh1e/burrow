#include "blueprint.h"
#include <quickjs.h>
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
    const struct bp_file *active[MAX_DEPTH];
    JSValue compiled[BP_MAX_FILES];
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
    fail(p, "blueprint-limit", "execution budget exhausted");
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

static bool integer_field(JSContext *ctx, JSValueConst obj, const char *key, size_t *out)
{
    JSValue value = JS_GetPropertyStr(ctx, obj, key);
    double n;
    bool ok = JS_IsNumber(value) && !JS_ToFloat64(ctx, &n, value) &&
              n >= 0 && n <= UINT32_MAX && (double)(uint32_t)n == n;
    if (ok) *out = (size_t)n;
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

void bp_plan_free(struct bp_plan *plan)
{
    if (!plan) return;
    for (size_t i = 0; i < plan->count; ++i) {
        struct bp_step *s = &plan->steps[i];
        free(s->source); free(s->path); free(s->from); free(s->to);
        free(s->name); free(s->version); free(s->entry); free(s->value);
    }
    plan->count = 0;
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
    if (!(p->bundle->required & (1u << s->op))) {
        fail(p, "undeclared-capability", location);
        return;
    }
    if (s->op == BP_COPY || s->op == BP_INSTALL) {
        char *ref = field(ctx, obj, "payload"), path[BP_DETAIL_CAP];
        if (ref && relative_path(module->path, ref, path)) s->payload = bp_find(p->bundle, path);
        free(ref);
        if (!s->payload || s->payload->module) { fail(p, "invalid-payload", location); return; }
    }
    if (s->op == BP_COPY || s->op == BP_REPLACE) {
        s->path = field(ctx, obj, "path");
        if (!s->path || !bp_safe_path(s->path)) { fail(p, "invalid-path", location); return; }
    }
    switch (s->op) {
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

/* Each invocation gets its own JSON inputs, even when an author reuses the
   same object for two includes. Child code can never mutate its caller's args. */
static JSValue snapshot_args(JSContext *ctx, JSValueConst args)
{
    JSValue json = JS_JSONStringify(ctx, args, JS_UNDEFINED, JS_UNDEFINED);
    size_t n = 0;
    const char *bytes = JS_IsString(json) ? JS_ToCStringLen(ctx, &n, json) : NULL;
    JSValue copy = bytes ? JS_ParseJSON(ctx, bytes, n, "<inputs>") : JS_UNDEFINED;
    if (bytes) JS_FreeCString(ctx, bytes);
    JS_FreeValue(ctx, json);
    return copy;
}

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
            JSValue bound = snapshot_args(ctx, args);
            if (!JS_IsObject(bound) || JS_IsArray(ctx, bound))
                fail(p, "invalid-arguments", position);
            else {
                const char *caller = p->call;
                p->call = call;
                expand(ctx, p, file, bound, call, depth + 1);
                p->call = caller;
            }
            JS_FreeValue(ctx, bound);
        }
    }
    JS_FreeValue(ctx, args);
    free(ref);
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
        else if (!strcmp(op, "include")) include(ctx, p, obj, file, position, depth);
        else add_step(ctx, p, obj, op, file, position);
        free(op);
        JS_FreeValue(ctx, obj);
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
                         .call = entry->path, .start = clock() };
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
        JSValue args = JS_NewObject(ctx);
        if (!p.error) expand(ctx, &p, entry, args, entry->path, 0);
        if (JS_IsJobPending(rt)) fail(&p, "async-blueprint", entry->path);
        JS_FreeValue(ctx, args);
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
