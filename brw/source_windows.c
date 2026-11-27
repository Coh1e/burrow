#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <stdlib.h>
#include "install_windows.h"
#include "shim_windows.h"
#include "source_windows.h"
#include "paths_windows.h"

enum { PATH_CAP = BRW_PATH_CAP, LINE_CAP = PATH_CAP + 512 };

enum native_payload_kind { NATIVE_EXECUTABLE, NATIVE_ARCHIVE };

struct native_package {
    const wchar_t *source;
    const wchar_t *name;
    const wchar_t *version;
    enum native_payload_kind kind;
    const wchar_t *location;
    const wchar_t *sha256;
    const wchar_t *entry;
    const wchar_t *depends_text;   /* Original spelling for CLI and package receipts. */
    const wchar_t *dependencies;  /* NUL-separated direct names, ended by an empty name. */
};

struct selection;
static bool collect(struct selection *selection, const struct native_package *package, size_t row_chars);

static bool path_join(wchar_t out[PATH_CAP], const wchar_t *base, const wchar_t *name)
{
    if (wcslen(base) + wcslen(name) + 2 > PATH_CAP)
        return false;
    return swprintf(out, PATH_CAP, L"%ls\\%ls", base, name) >= 0;
}

static bool absolute_path(const wchar_t *path)
{
    return wcslen(path) >= 3 && path[1] == L':' &&
           (path[2] == L'\\' || path[2] == L'/');
}

static bool directory(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

static bool ensure_directory(const wchar_t *path)
{
    return CreateDirectoryW(path, NULL) || directory(path);
}

static const char *source_root(wchar_t root[PATH_CAP], bool create)
{
    wchar_t config[PATH_CAP];
    const char *error = brw_path(BRW_CONFIG, config, create);
    if (error)
        return error;
    return path_join(root, config, L"sources") && (!create || ensure_directory(root))
        ? NULL : "source-directory";
}

/* A line is bounded and decoded strictly. CRLF and LF are both accepted. */
static int read_line(FILE *file, wchar_t line[LINE_CAP])
{
    char bytes[LINE_CAP * 4];
    size_t length = 0;
    int byte;
    while ((byte = fgetc(file)) != EOF && byte != '\n') {
        if (!byte || length == sizeof bytes - 1)
            return -1;
        bytes[length++] = (char)byte;
    }
    if (ferror(file))
        return -1;
    if (!length && byte == EOF)
        return 0;
    if (length && bytes[length - 1] == '\r')
        --length;
    bytes[length] = 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, -1, line, LINE_CAP) ? 1 : -1;
}

static wchar_t *take_field(wchar_t **cursor)
{
    wchar_t *field = *cursor;
    wchar_t *end = field + wcscspn(field, L"\t");
    *cursor = *end ? end + 1 : end;
    *end = 0;
    return field;
}

/* The description borrows the source, decoded line and dependency buffer until the next read. */
static bool parse_package(const wchar_t *source, wchar_t *line, struct native_package *package,
                          wchar_t names[BRW_DEPENDENCIES_CAP + 1])
{
    package->source = source;
    package->name = take_field(&line);
    package->version = take_field(&line);
    const wchar_t *kind = take_field(&line);
    if (!wcscmp(kind, L"exe")) package->kind = NATIVE_EXECUTABLE;
    else if (!wcscmp(kind, L"archive")) package->kind = NATIVE_ARCHIVE;
    else return false;
    package->location = take_field(&line);
    package->sha256 = take_field(&line);
    package->entry = take_field(&line);
    package->depends_text = line;
    struct local_package payload = {package->name, package->version, package->location,
                                    package->sha256, package->entry};
    if (brw_validate_source_package(&payload, package->kind == NATIVE_EXECUTABLE) ||
        !(brw_http_location(package->location) || brw_package_component(package->location)) ||
        !brw_dependency_names(line))
        return false;
    size_t length = wcslen(line);
    wmemcpy(names, line, length + 1);
    names[length + 1] = 0;
    for (size_t i = 0; i < length; ++i)
        if (names[i] == L',') names[i] = 0;
    package->dependencies = names;
    return true;
}

static const char *read_catalog(const wchar_t *path, const wchar_t *source,
                                struct selection *selection)
{
    FILE *file = _wfsopen(path, L"rb", _SH_DENYWR);
    if (!file)
        return "catalog-open";
    wchar_t line[LINE_CAP], names[BRW_DEPENDENCIES_CAP + 1];
    const char *error = NULL;
    if (read_line(file, line) != 1 || wcscmp(line, L"brw-catalog-v1")) {
        error = "catalog-format";
    } else {
        int status;
        while ((status = read_line(file, line)) == 1) {
            size_t row_chars = wcslen(line) + 1;
            struct native_package package;
            if (!parse_package(source, line, &package, names)) {
                error = "catalog-format";
                break;
            }
            if (selection && !collect(selection, &package, row_chars)) {
                error = "catalog-memory";
                break;
            }
        }
        if (status < 0)
            error = "catalog-format";
    }
    fclose(file);
    return error;
}

static int report(bool json, const char *error, const char *success)
{
    if (error && json)
        printf("{\"status\":\"failed\",\"error\":\"%s\",\"step\":0}\n", error);
    else if (error)
        fprintf(stderr, "brw: %s\n", error);
    else if (json)
        printf("{\"status\":\"%s\"}\n", success);
    else
        puts(success);
    return error ? 1 : 0;
}

static bool source_location(const wchar_t *record, wchar_t catalog[PATH_CAP])
{
    DWORD attrs = GetFileAttributesW(record);
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;
    FILE *file = _wfsopen(record, L"rb", _SH_DENYWR);
    if (!file)
        return false;
    size_t bytes = fread(catalog, 1, PATH_CAP * sizeof *catalog, file);
    size_t count = bytes / sizeof *catalog;
    bool valid = bytes % sizeof *catalog == 0 && count > 1 && catalog[count - 1] == 0 && wcslen(catalog) == count - 1 &&
                 absolute_path(catalog) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return valid;
}

static const char *add_source(const wchar_t *name, const wchar_t *input, bool preview)
{
    wchar_t catalog[PATH_CAP], root[PATH_CAP], record[PATH_CAP], stage[PATH_CAP];
    DWORD size = GetFullPathNameW(input, PATH_CAP, catalog, NULL);
    if (!size || size >= PATH_CAP)
        return "catalog-path";
    const char *error = read_catalog(catalog, name, NULL);
    if (error || preview)
        return error;
    error = source_root(root, true);
    if (error)
        return error;
    if (!path_join(record, root, name))
        return "source-directory";
    /* Stage beside the registry so a concurrent scan cannot see a partial record. */
    wchar_t *separator = wcsrchr(root, L'\\');
    *separator = 0;
    if (!GetTempFileNameW(root, L"brw", 0, stage))
        return "source-stage";
    FILE *file = _wfopen(stage, L"wb");
    if (!file) {
        DeleteFileW(stage);
        return "source-stage";
    }
    size_t length = wcslen(catalog) + 1;
    bool written = fwrite(catalog, sizeof *catalog, length, file) == length;
    if (fclose(file) || !written)
        error = "source-stage";
    else if (!MoveFileW(stage, record))
        error = GetLastError() == ERROR_ALREADY_EXISTS ? "source-exists" : "source-write";
    if (error)
        DeleteFileW(stage);
    return error;
}

struct match {
    struct match *next;
    struct match *install_next;
    enum { UNVISITED, VISITING, PLANNED } visit;
    struct native_package package;
    const wchar_t *catalog;
    wchar_t strings[];
};

struct selection {
    const wchar_t *query;
    const wchar_t *qualified;
    const wchar_t *catalog;
    bool search;
    brw_catalog_visitor visitor;
    void *context;
    struct match *first;
    struct match **tail;
};

static bool contains(const wchar_t *text, const wchar_t *query)
{
    size_t length = wcslen(query);
    do {
        if (!_wcsnicmp(text, query, length))
            return true;
    } while (*text++);
    return false;
}

static wchar_t *copy_string(wchar_t **storage, const wchar_t *value)
{
    size_t count = wcslen(value) + 1;
    wchar_t *copy = wmemcpy(*storage, value, count);
    *storage += count;
    return copy;
}

static bool collect(struct selection *selection, const struct native_package *package, size_t row_chars)
{
    if (selection->search ? !contains(package->name, selection->query)
                          : _wcsicmp(package->name, selection->query) != 0)
        return true;
    /* The original line covers the retained field strings, including their terminators.
       Also retain the source, catalog path and the separately decoded dependency names. */
    size_t names_chars = wcslen(package->depends_text) + 2;
    size_t characters = row_chars + (wcslen(package->source) + 1) +
                        (wcslen(selection->catalog) + 1) + names_chars;
    struct match *match = malloc(sizeof *match + characters * sizeof *match->strings);
    if (!match)
        return false;
    match->next = NULL;
    match->install_next = NULL;
    match->visit = UNVISITED;
    wchar_t *next = match->strings;
    match->catalog = copy_string(&next, selection->catalog);
    match->package.source = copy_string(&next, package->source);
    match->package.name = copy_string(&next, package->name);
    match->package.version = copy_string(&next, package->version);
    match->package.kind = package->kind;
    match->package.location = copy_string(&next, package->location);
    match->package.sha256 = copy_string(&next, package->sha256);
    match->package.entry = copy_string(&next, package->entry);
    match->package.depends_text = copy_string(&next, package->depends_text);
    match->package.dependencies = wmemcpy(next, package->dependencies, names_chars);
    *selection->tail = match;
    selection->tail = &match->next;
    return true;
}

static void print_field(const char *key, const wchar_t *value)
{
    printf("\"%s\":", key);
    brw_json_string(value);
}

static void print_package(const struct native_package *package, bool json)
{
    const wchar_t *kind = package->kind == NATIVE_EXECUTABLE ? L"exe" : L"archive";
    if (!json) {
        printf("%ls/%ls %ls %ls %ls %ls %ls", package->source, package->name, package->version,
               kind, package->location, package->sha256, package->entry);
        if (*package->depends_text)
            printf(" depends=%ls", package->depends_text);
        putchar('\n');
        return;
    }
    const struct { const char *key; const wchar_t *value; } fields[] = {
        {"source", package->source}, {"name", package->name}, {"version", package->version},
        {"kind", kind}, {"location", package->location}, {"sha256", package->sha256},
        {"entry", package->entry}
    };
    putchar('{');
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
        if (i) putchar(',');
        print_field(fields[i].key, fields[i].value);
    }
    if (*package->depends_text) {
        putchar(',');
        print_field("depends", package->depends_text);
    }
    putchar('}');
}

static const char *scan_sources(const wchar_t *root, struct selection *selection, bool json)
{
    wchar_t pattern[PATH_CAP];
    if (!path_join(pattern, root, L"*")) {
        if (!selection && json)
            report(true, "source-directory", NULL);
        return "source-directory";
    }
    if (!selection && json)
        fputs("{\"sources\":[", stdout);
    const char *error = NULL;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search != INVALID_HANDLE_VALUE) {
        bool comma = false;
        do {
            if (!brw_package_component(data.cFileName) ||
                (selection && selection->qualified && _wcsicmp(selection->qualified, data.cFileName)))
                continue;
            wchar_t record[PATH_CAP], catalog[PATH_CAP];
            if (!path_join(record, root, data.cFileName) || !source_location(record, catalog)) {
                error = "source-record";
                break;
            }
            if (selection && selection->visitor) {
                error = selection->visitor(data.cFileName, catalog, selection->context);
                if (error) break;
            } else if (selection) {
                selection->catalog = catalog;
                error = read_catalog(catalog, data.cFileName, selection);
                selection->catalog = NULL;
                if (error)
                    break;
            } else if (json) {
                printf("%s{", comma ? "," : "");
                print_field("name", data.cFileName);
                putchar(',');
                print_field("catalog", catalog);
                putchar('}');
                comma = true;
            } else {
                printf("%ls %ls\n", data.cFileName, catalog);
            }
        } while (FindNextFileW(search, &data));
        if (!error && GetLastError() != ERROR_NO_MORE_FILES)
            error = "source-directory";
        FindClose(search);
    } else if (GetLastError() != ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = "source-directory";
    }
    if (!selection && json) {
        putchar(']');
        if (error)
            printf(",\"status\":\"failed\",\"error\":\"%s\",\"step\":0", error);
        else
            fputs(",\"status\":\"ok\"", stdout);
        puts("}");
    }
    return error;
}

static const char *run_match(const struct match *match, const struct digby_plan *plan, size_t *step)
{
    wchar_t location[PATH_CAP], base[PATH_CAP];
    const struct native_package *native = &match->package;
    const wchar_t *input = native->location;
    if (brw_package_component(input)) {
        wcscpy(base, match->catalog);
        wchar_t *separator = wcsrchr(base, L'\\');
        if (!separator)
            return "catalog-path";
        *separator = 0;
        if (!path_join(location, base, input))
            return "download-path";
        input = location;
    }
    struct local_package package = {native->name, native->version, input, native->sha256, native->entry};
    const char *error = brw_install_with_dependencies(&package, native->kind == NATIVE_EXECUTABLE,
                                                       native->depends_text, plan, step);
    if (error)
        ++*step;
    return error;
}

static void free_matches(struct match *match)
{
    while (match) {
        struct match *next = match->next;
        free(match);
        match = next;
    }
}

struct dependency_plan {
    struct digby_plan operations;
    const wchar_t *root;
    struct match *target;
    struct selection catalog;
    struct match *order;
    struct match **tail;
    wchar_t problem[BRW_PACKAGE_NAME_CAP];
};

static const char *resolve_dependencies(struct dependency_plan *plan, struct match *match,
                                        unsigned depth)
{
    if (match->visit == PLANNED)
        return NULL;
    if (match->visit == VISITING || depth == 256) {
        wcscpy(plan->problem, match->package.name);
        return match->visit == VISITING ? "dependency-cycle" : "dependency-depth";
    }
    match->visit = VISITING;
    for (const wchar_t *name = match->package.dependencies; *name; name += wcslen(name) + 1) {
        if (!_wcsicmp(name, plan->target->package.name)) {
            wcscpy(plan->problem, name);
            return "dependency-cycle";
        }
        if (brw_is_installed(name))
            continue;
        if (!plan->catalog.tail) {
            plan->catalog.tail = &plan->catalog.first;
            plan->catalog.query = L"";
            plan->catalog.search = true;
            const char *error = scan_sources(plan->root, &plan->catalog, false);
            if (error)
                return error;
        }
        struct match *dependency = NULL;
        for (struct match *item = plan->catalog.first; item; item = item->next) {
            if (_wcsicmp(name, item->package.name))
                continue;
            if (dependency) {
                wcscpy(plan->problem, name);
                return "dependency-ambiguous";
            }
            dependency = item;
        }
        if (!dependency) {
            wcscpy(plan->problem, name);
            return "dependency-not-found";
        }
        const char *error = resolve_dependencies(plan, dependency, depth + 1);
        if (error)
            return error;
    }
    match->visit = PLANNED;
    *plan->tail = match;
    plan->tail = &match->install_next;
    return NULL;
}

const char *brw_visit_catalogs(brw_catalog_visitor visitor, void *context)
{
    wchar_t root[PATH_CAP];
    const char *error = source_root(root, false);
    struct selection selection = {.visitor = visitor, .context = context};
    return error ? error : scan_sources(root, &selection, false);
}

static int install_match(const wchar_t *root, struct match *target, bool json, bool preview)
{
    struct dependency_plan plan = {.root = root, .target = target, .operations = digby_source_install_plan()};
    plan.tail = &plan.order;
    /* An install resolves the current package order while holding the mutation lock. */
    const char *error = preview ? NULL : brw_begin_package_changes();
    bool locked = !preview && !error;
    if (!error)
        error = resolve_dependencies(&plan, target, 0);
    size_t step = 0;
    bool has_dependencies = *target->package.dependencies != 0;
    if (!error && !preview) {
        /* An existing main package must not trigger writes to its dependencies. */
        if (has_dependencies && brw_is_installed(target->package.name))
            error = "destination-exists";
        bool cancellable = false;
        if (!error) {
            cancellable = brw_begin_source_install();
            if (!cancellable)
                error = "cancel-handler";
        }
        for (struct match *item = plan.order; !error && item; item = item->install_next) {
            error = run_match(item, &plan.operations, &step);
            if (error)
                wcscpy(plan.problem, item->package.name);
        }
        if (cancellable)
            brw_end_source_install();
    }
    if (locked)
        brw_end_package_changes();
    if (!has_dependencies || (!error && !preview)) {
        free_matches(plan.catalog.first);
        return brw_report_plan(json, preview, "installed", &plan.operations, error, step);
    }
    if (error) {
        if (json) {
            printf("{\"status\":\"failed\",\"error\":\"%s\",\"step\":%zu,\"package\":", error, step);
            brw_json_string(*plan.problem ? plan.problem : target->package.name);
            puts("}");
        } else {
            fprintf(stderr, "brw: %s (%ls)\n", error,
                    *plan.problem ? plan.problem : target->package.name);
        }
    } else if (json) {
        fputs("{\"status\":\"planned\",", stdout);
        brw_print_plan_operations(&plan.operations, true, true);
        fputs(",\"packages\":[", stdout);
        for (struct match *item = plan.order; item; item = item->install_next) {
            if (item != plan.order)
                putchar(',');
            brw_json_string(item->package.name);
        }
        puts("]}");
    } else {
        for (struct match *item = plan.order; item; item = item->install_next)
            printf("Install %ls\n", item->package.name);
        brw_print_plan_operations(&plan.operations, false, true);
    }
    free_matches(plan.catalog.first);
    return error ? 1 : 0;
}

static int package_query(const wchar_t *verb, const wchar_t *root, const wchar_t *query,
                         bool json, bool preview)
{
    struct selection selection = {0};
    selection.search = !wcscmp(verb, L"search");
    selection.query = query;
    selection.tail = &selection.first;
    wchar_t qualified[128];
    const wchar_t *slash = selection.search ? NULL : wcschr(query, L'/');
    if (slash) {
        size_t length = (size_t)(slash - query);
        if (length >= sizeof qualified / sizeof *qualified)
            return report(json, "invalid-source", NULL);
        wmemcpy(qualified, query, length);
        qualified[length] = 0;
        if (!brw_package_component(qualified))
            return report(json, "invalid-source", NULL);
        selection.qualified = qualified;
        selection.query = slash + 1;
    }
    const char *error = scan_sources(root, &selection, json);
    struct match *first = selection.first;
    if (!error && !selection.search && (!first || first->next))
        error = first ? "package-ambiguous" : "package-not-found";
    int result = 0;
    if (error) {
        result = report(json, error, NULL);
    } else if (!wcscmp(verb, L"install")) {
        result = install_match(root, first, json, preview);
    } else {
        if (json)
            fputs(selection.search ? "{\"status\":\"ok\",\"packages\":[" :
                  "{\"status\":\"ok\",\"package\":", stdout);
        for (struct match *match = first; match; match = match->next) {
            if (json && match != first)
                putchar(',');
            print_package(&match->package, json);
        }
        if (json)
            puts(selection.search ? "]}" : "}");
    }
    free_matches(selection.first);
    return result;
}

int brw_source_command(const wchar_t *verb, size_t count, const wchar_t **args,
                       bool json, bool preview)
{
    if (!wcscmp(verb, L"list") && !count && !preview)
        return brw_list_installed(json);
    wchar_t root[PATH_CAP];
    const char *error = source_root(root, false);
    if (error)
        return report(json, error, NULL);
    if (!wcscmp(verb, L"source")) {
        if (count == 1 && !wcscmp(args[0], L"list") && !preview) {
            const char *error = scan_sources(root, NULL, json);
            if (error && !json)
                return report(false, error, NULL);
            return error ? 1 : 0;
        }
        if (count >= 2 && !brw_package_component(args[1]))
            return report(json, "invalid-source", NULL);
        if (count == 3 && !wcscmp(args[0], L"add"))
            return report(json, add_source(args[1], args[2], preview), preview ? "planned" : "added");
        if (count == 2 && !wcscmp(args[0], L"remove")) {
            wchar_t record[PATH_CAP], catalog[PATH_CAP];
            if (!path_join(record, root, args[1]) || !source_location(record, catalog))
                return report(json, "source-not-found", NULL);
            return report(json, preview || DeleteFileW(record) ? NULL : "source-remove",
                          preview ? "planned" : "removed");
        }
    } else if (!count && !preview && !wcscmp(verb, L"search")) {
        return package_query(verb, root, L"", json, false);
    } else if (count == 1 && (!preview || !wcscmp(verb, L"install"))) {
        return package_query(verb, root, args[0], json, preview);
    }
    report(json, "invalid-arguments", NULL);
    return 2;
}
