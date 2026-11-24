#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <stdlib.h>
#include "install_windows.h"
#include "shim_windows.h"
#include "source_windows.h"

enum { PATH_CAP = BRW_PATH_CAP, LINE_CAP = PATH_CAP + 512 };

enum catalog_field {
    PACKAGE_NAME, PACKAGE_VERSION, PAYLOAD_KIND, PAYLOAD_LOCATION, PAYLOAD_SHA256,
    PACKAGE_ENTRY, CATALOG_FIELDS
};

struct catalog_row {
    wchar_t text[LINE_CAP];
    wchar_t *field[CATALOG_FIELDS];
};

typedef bool (*visit_package)(const wchar_t *source, const struct catalog_row *row, void *context);

void brw_json_string(const wchar_t *value)
{
    putchar('"');
    for (; *value; ++value) {
        unsigned ch = *value;
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 32 || ch > 126)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}

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

static bool source_root(wchar_t root[PATH_CAP], bool create)
{
    wchar_t profile[PATH_CAP], config[PATH_CAP], base[PATH_CAP];
    DWORD size = GetEnvironmentVariableW(L"XDG_CONFIG_HOME", config, PATH_CAP);
    if (!size || (size < PATH_CAP && !absolute_path(config))) {
        size = GetEnvironmentVariableW(L"USERPROFILE", profile, PATH_CAP);
        if (!size || size >= PATH_CAP || !absolute_path(profile) ||
            !path_join(config, profile, L".config"))
            return false;
    } else if (size >= PATH_CAP) {
        return false;
    }
    return path_join(base, config, L"burrow") && path_join(root, base, L"sources") &&
           (!create || (ensure_directory(config) && ensure_directory(base) && ensure_directory(root)));
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

static bool parse_row(struct catalog_row *row)
{
    wchar_t *field = row->text;
    for (size_t i = 0; i < CATALOG_FIELDS; ++i) {
        row->field[i] = field;
        wchar_t *tab = wcschr(field, L'\t');
        if ((tab != NULL) != (i + 1 < CATALOG_FIELDS))
            return false;
        if (tab)
            *tab = 0;
        if (!*field)
            return false;
        if (tab)
            field = tab + 1;
    }
    struct local_package package = {row->field[PACKAGE_NAME], row->field[PACKAGE_VERSION], row->field[PAYLOAD_LOCATION],
                                    row->field[PAYLOAD_SHA256], row->field[PACKAGE_ENTRY]};
    const wchar_t *location = row->field[PAYLOAD_LOCATION];
    bool executable = !wcscmp(row->field[PAYLOAD_KIND], L"exe");
    return (executable || !wcscmp(row->field[PAYLOAD_KIND], L"archive")) &&
           !brw_validate_source_package(&package, executable) &&
           (brw_http_location(location) || brw_package_component(location));
}

static const char *read_catalog(const wchar_t *path, const wchar_t *source,
                                visit_package visit, void *context)
{
    FILE *file = _wfsopen(path, L"rb", _SH_DENYWR);
    if (!file)
        return "catalog-open";
    struct catalog_row row;
    const char *error = NULL;
    if (read_line(file, row.text) != 1 || wcscmp(row.text, L"brw-catalog-v1")) {
        error = "catalog-format";
    } else {
        int status;
        while ((status = read_line(file, row.text)) == 1) {
            if (!parse_row(&row)) {
                error = "catalog-format";
                break;
            }
            if (visit && !visit(source, &row, context)) {
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
    const char *error = read_catalog(catalog, name, NULL, NULL);
    if (error || preview)
        return error;
    if (!source_root(root, true) || !path_join(record, root, name))
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
    const wchar_t *source;
    const wchar_t *catalog;
    const wchar_t *field[CATALOG_FIELDS];
    wchar_t strings[];
};

struct selection {
    const wchar_t *query;
    const wchar_t *qualified;
    const wchar_t *catalog;
    bool search;
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

static bool collect(const wchar_t *source, const struct catalog_row *row, void *context)
{
    struct selection *selection = context;
    if (selection->search ? !contains(row->field[PACKAGE_NAME], selection->query)
                          : _wcsicmp(row->field[PACKAGE_NAME], selection->query) != 0)
        return true;
    size_t source_length = wcslen(source) + 1;
    size_t catalog_length = wcslen(selection->catalog) + 1;
    size_t field_length[CATALOG_FIELDS];
    size_t characters = source_length + catalog_length;
    for (size_t i = 0; i < CATALOG_FIELDS; ++i) {
        field_length[i] = wcslen(row->field[i]) + 1;
        characters += field_length[i];
    }
    struct match *match = malloc(sizeof *match + characters * sizeof *match->strings);
    if (!match)
        return false;
    match->next = NULL;
    wchar_t *next = match->strings;
    match->source = wmemcpy(next, source, source_length);
    next += source_length;
    match->catalog = wmemcpy(next, selection->catalog, catalog_length);
    next += catalog_length;
    for (size_t i = 0; i < CATALOG_FIELDS; ++i) {
        match->field[i] = wmemcpy(next, row->field[i], field_length[i]);
        next += field_length[i];
    }
    *selection->tail = match;
    selection->tail = &match->next;
    return true;
}

static void print_field(const char *key, const wchar_t *value)
{
    printf("\"%s\":", key);
    brw_json_string(value);
}

static void print_package(const struct match *match, bool json)
{
    const wchar_t *const *field = match->field;
    if (!json) {
        printf("%ls/%ls %ls %ls %ls %ls %ls\n", match->source, field[PACKAGE_NAME], field[PACKAGE_VERSION],
               field[PAYLOAD_KIND], field[PAYLOAD_LOCATION], field[PAYLOAD_SHA256], field[PACKAGE_ENTRY]);
        return;
    }
    static const char *keys[CATALOG_FIELDS] = {"name", "version", "kind", "location", "sha256", "entry"};
    putchar('{');
    print_field("source", match->source);
    for (size_t i = 0; i < CATALOG_FIELDS; ++i) {
        putchar(',');
        print_field(keys[i], field[i]);
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
            if (selection) {
                selection->catalog = catalog;
                error = read_catalog(catalog, data.cFileName, collect, selection);
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

static int install_match(const struct match *match, bool json, bool preview)
{
    wchar_t location[PATH_CAP], base[PATH_CAP];
    const wchar_t *const *field = match->field;
    const wchar_t *input = field[PAYLOAD_LOCATION];
    if (brw_package_component(input)) {
        wcscpy(base, match->catalog);
        wchar_t *separator = wcsrchr(base, L'\\');
        if (!separator)
            return report(json, "catalog-path", NULL);
        *separator = 0;
        if (!path_join(location, base, input))
            return report(json, "download-path", NULL);
        input = location;
    }
    struct local_package package = {field[PACKAGE_NAME], field[PACKAGE_VERSION], input, field[PAYLOAD_SHA256], field[PACKAGE_ENTRY]};
    struct digby_plan plan = digby_source_install_plan();
    size_t step = 0;
    const char *error = preview ? NULL :
        brw_install_source_package(&package, !wcscmp(field[PAYLOAD_KIND], L"exe"), &plan, &step);
    return brw_report_plan(json, preview, "installed", &plan, error, error ? step + 1 : 0);
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
        result = install_match(first, json, preview);
    } else {
        if (json)
            fputs(selection.search ? "{\"status\":\"ok\",\"packages\":[" :
                  "{\"status\":\"ok\",\"package\":", stdout);
        for (struct match *match = first; match; match = match->next) {
            if (json && match != first)
                putchar(',');
            print_package(match, json);
        }
        if (json)
            puts(selection.search ? "]}" : "}");
    }
    while (selection.first) {
        struct match *next = selection.first->next;
        free(selection.first);
        selection.first = next;
    }
    return result;
}

int brw_source_command(const wchar_t *verb, size_t count, const wchar_t **args,
                       bool json, bool preview)
{
    if (!wcscmp(verb, L"list") && !count && !preview)
        return brw_list_installed(json);
    wchar_t root[PATH_CAP];
    if (!source_root(root, false))
        return report(json, "source-directory", NULL);
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
