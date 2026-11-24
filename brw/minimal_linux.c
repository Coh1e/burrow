#define _POSIX_C_SOURCE 200809L
#include "literal.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Publish a complete sibling temporary file only if the destination is free. */
static FILE *stage(const char *path, char **temporary)
{
    const char suffix[] = ".brw.XXXXXX";
    const char *slash = strrchr(path, '/');
    size_t prefix = slash ? (size_t)(slash + 1 - path) : 0;
    *temporary = malloc(prefix + sizeof suffix);
    if (!*temporary)
        return NULL;
    memcpy(*temporary, path, prefix);
    memcpy(*temporary + prefix, suffix, sizeof suffix);
    int fd = mkstemp(*temporary);
    if (fd < 0)
        return NULL;
    FILE *stream = fdopen(fd, "wb");
    if (!stream) {
        close(fd);
        unlink(*temporary);
    }
    return stream;
}

static int publish(FILE *stream, char *temporary, const char *path, int failed)
{
    if (fclose(stream) != 0)
        failed = 1;
    if (!failed && link(temporary, path) != 0)
        failed = 1;
    unlink(temporary);
    free(temporary);
    return failed;
}

static int text_operation(char **args, const char *replacement, const char *path,
                          const size_t *expected)
{
    FILE *input = fopen(args[0], "rb");
    if (!input)
        return 1;
    char *text = NULL;
    size_t capacity = 0;
    ssize_t length = getdelim(&text, &capacity, 0, input);
    int failed = !feof(input) || ferror(input);
    fclose(input);
    char *temporary = NULL;
    if (!failed) {
        FILE *out = path ? stage(path, &temporary) : stdout;
        if (out) {
            failed = brw_literal(out, length < 0 ? "" : text, args[1], replacement, expected);
            failed = path ? publish(out, temporary, path, failed) : (fflush(out) || failed);
        } else {
            free(temporary);
            failed = 1;
        }
    }
    free(text);
    return failed;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int pack_entry(struct archive *writer, const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return 1;
    struct stat info;
    struct archive_entry *entry = archive_entry_new();
    int failed = !entry || fstat(fd, &info) || !S_ISREG(info.st_mode);
    if (!failed) {
        archive_entry_set_pathname(entry, basename_of(path));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_entry_set_size(entry, info.st_size);
        failed = archive_write_header(writer, entry) != ARCHIVE_OK;
        char buffer[16384];
        off_t total = 0;
        ssize_t size = 0;
        while (!failed && (size = read(fd, buffer, sizeof buffer)) > 0) {
            failed = size > info.st_size - total || archive_write_data(writer, buffer, size) != size;
            if (!failed)
                total += size;
        }
        failed = failed || size < 0 || total != info.st_size;
    }
    archive_entry_free(entry);
    close(fd);
    return failed;
}

static int pack(const char *path, int count, char **inputs)
{
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < i; ++j)
            if (!strcmp(basename_of(inputs[i]), basename_of(inputs[j])))
                return 1;
    char *temporary;
    FILE *out = stage(path, &temporary);
    if (!out) {
        free(temporary);
        return 1;
    }
    struct archive *writer = archive_write_new();
    int failed = !writer || archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK ||
                 archive_write_add_filter_gzip(writer) != ARCHIVE_OK ||
                 archive_write_open_fd(writer, fileno(out)) != ARCHIVE_OK;
    for (int i = 0; !failed && i < count; ++i)
        failed = pack_entry(writer, inputs[i]);
    if (writer && archive_write_close(writer) != ARCHIVE_OK)
        failed = 1;
    archive_write_free(writer);
    return publish(out, temporary, path, failed);
}

static int child_directory(int parent, const char *name)
{
    if (mkdirat(parent, name, 0700) && errno != EEXIST)
        return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

static int extract_file(struct archive *reader, int parent, const char *name)
{
    /* The CLI handles one command per process. The archive is already open
       and subsequent directory lookup stays relative to the pinned root fd. */
    if (fchdir(parent))
        return 1;
    char *temporary;
    FILE *out = stage(name, &temporary);
    if (!out) {
        free(temporary);
        return 1;
    }
    int failed = archive_read_data_into_fd(reader, fileno(out)) != ARCHIVE_OK;
    return publish(out, temporary, name, failed);
}

static int extract_entry(struct archive *reader, struct archive_entry *entry, int root)
{
    const char *name = archive_entry_pathname(entry);
    int directory = archive_entry_filetype(entry) == AE_IFDIR;
    if (!name || !*name || *name == '/' || archive_entry_hardlink(entry) ||
        (!directory && archive_entry_filetype(entry) != AE_IFREG))
        return 1;
    for (const char *part = name; *part;) {
        size_t length = strcspn(part, "/");
        if (length == 2 && !strncmp(part, "..", 2))
            return 1;
        part += length;
        if (*part) ++part;
    }
    char *copy = strdup(name);
    if (!copy)
        return 1;
    int parent = dup(root);
    char *state, *part = strtok_r(copy, "/", &state);
    int failed = parent < 0 || !part;
    while (!failed && part) {
        char *next = strtok_r(NULL, "/", &state);
        if (next || directory) {
            int child = child_directory(parent, part);
            close(parent);
            parent = child;
            failed = child < 0;
        } else {
            failed = extract_file(reader, parent, part);
        }
        part = next;
    }
    if (parent >= 0)
        close(parent);
    free(copy);
    return failed;
}

static int unpack(const char *path, const char *destination)
{
    int root = open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0)
        return 1;
    struct archive *reader = archive_read_new();
    int failed = !reader || archive_read_support_format_tar(reader) != ARCHIVE_OK ||
                 archive_read_support_filter_gzip(reader) != ARCHIVE_OK ||
                 archive_read_open_filename(reader, path, 16384) != ARCHIVE_OK;
    while (!failed) {
        struct archive_entry *entry;
        int status = archive_read_next_header(reader, &entry);
        if (status == ARCHIVE_EOF)
            break;
        failed = status != ARCHIVE_OK || extract_entry(reader, entry, root);
    }
    if (archive_read_free(reader) != ARCHIVE_OK)
        failed = 1;
    close(root);
    return failed;
}

int main(int argc, char **argv)
{
    /* libarchive converts POSIX pathnames to and from UTF-8 PAX headers. */
    if (!setlocale(LC_CTYPE, "C.UTF-8"))
        setlocale(LC_CTYPE, "");
    int failed;
    if (argc == 4 && !strcmp(argv[1], "search")) {
        failed = text_operation(argv + 2, NULL, NULL, NULL);
    } else if ((argc == 6 || argc == 7) && !strcmp(argv[1], "replace")) {
        size_t count = 0;
        if (argc == 7) {
            const char *number = argv[6];
            if (!*number || number[strspn(number, "0123456789")])
                return 2;
            errno = 0;
            count = strtoul(number, NULL, 10);
            if (errno)
                return 2;
        }
        failed = text_operation(argv + 2, argv[4], argv[5], argc == 7 ? &count : NULL);
    } else if (argc >= 4 && !strcmp(argv[1], "pack")) {
        failed = pack(argv[2], argc - 3, argv + 3);
    } else if (argc == 4 && !strcmp(argv[1], "unpack")) {
        failed = unpack(argv[2], argv[3]);
    } else {
        fputs("usage: brw search FILE TEXT | replace FILE FROM TO NEWFILE [COUNT] | "
              "pack ARCHIVE FILE... | unpack ARCHIVE DIRECTORY\n", stderr);
        return 2;
    }
    if (failed)
        fprintf(stderr, "%s failed for %s\n", argv[1], argv[2]);
    return failed != 0;
}
