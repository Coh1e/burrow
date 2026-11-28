#define _POSIX_C_SOURCE 200809L
#include "blueprint_bundle_io.h"
#include "blueprint_files_posix.h"
#include "verified_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool brw_valid_ipv6_literal(const char *address)
{
    struct in6_addr parsed;
    return address && inet_pton(AF_INET6, address, &parsed) == 1;
}

struct bp_directory *bp_directory_open(const char *path)
{
    struct bp_directory *dir = malloc(sizeof *dir);
    if (!dir) return NULL;
    dir->fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir->fd < 0) { free(dir); return NULL; }
    return dir;
}

void bp_directory_close(struct bp_directory *dir)
{
    close(dir->fd);
    free(dir);
}

static int open_file(const struct bp_directory *dir, const char *path)
{
    char copy[256];
    if (strlen(path) >= sizeof copy) return -1;
    strcpy(copy, path);
    int parent = dir->fd;
    char *name = copy;
    int fd;
    for (;;) {
        char *slash = strchr(name, '/');
        if (slash) *slash = 0;
        fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                    (slash ? O_DIRECTORY : O_NONBLOCK));
        if (parent != dir->fd) close(parent);
        if (fd < 0 || !slash) break;
        parent = fd;
        name = slash + 1;
    }
    return fd;
}

static const char *read_bytes(int fd, bool reject_hard_links, size_t limit, unsigned char **out, size_t *size)
{
    struct stat st;
    unsigned char *bytes = NULL;
    const char *error = "bundle-file-unavailable";
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (reject_hard_links && st.st_nlink != 1) || st.st_size < 0)
        goto done;
    error = "bundle-limit";
    if ((uintmax_t)st.st_size > limit) goto done;
    size_t length = (size_t)st.st_size;
    error = "out-of-memory";
    bytes = malloc(length + 1);
    if (!bytes) goto done;
    size_t used = 0;
    error = "bundle-file-unavailable";
    for (;;) {
        ssize_t n = read(fd, bytes + used, length + 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (size_t)n > length - used) goto done;
        if (!n) break;
        used += (size_t)n;
    }
    if (used != length) goto done;
    bytes[length] = 0;
    *out = bytes;
    *size = length;
    bytes = NULL;
    error = NULL;
done:
    free(bytes);
    close(fd);
    return error;
}

const char *brw_read_local_file(const char *path, size_t limit, unsigned char **out, size_t *size)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    return fd < 0 ? "archive-unavailable" : read_bytes(fd, false, limit, out, size);
}

const char *bp_directory_read(const struct bp_directory *dir, const char *path,
                              size_t limit, unsigned char **out, size_t *size)
{
    int fd = open_file(dir, path);
    return fd < 0 ? "bundle-file-unavailable" : read_bytes(fd, true, limit, out, size);
}
