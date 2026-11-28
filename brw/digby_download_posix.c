#define _POSIX_C_SOURCE 200809L
#include "digby_download.h"
#include "digby_download_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct digby_download_control {
    struct sigaction previous_int, previous_term;
    volatile sig_atomic_t interrupted;
};

static struct digby_download_control foreground;

/* Run the same lookup for preflight and launch. Empty PATH components search
   the current directory. An unset PATH uses the platform's default search. */
static bool find_curl(char path[PATH_MAX])
{
    const char *dir = getenv("PATH");
    char default_path[PATH_MAX];
    if (!dir) {
        size_t size = confstr(_CS_PATH, default_path, sizeof default_path);
        if (!size || size > sizeof default_path) return false;
        dir = default_path;
    }
    for (;;) {
        size_t length = strcspn(dir, ":");
        if (length <= PATH_MAX - sizeof "/curl") {
            size_t prefix = length ? length : 1;
            memcpy(path, length ? dir : ".", prefix);
            memcpy(path + prefix, "/curl", sizeof "/curl");
            struct stat info;
            if (!stat(path, &info) && S_ISREG(info.st_mode) &&
                !faccessat(AT_FDCWD, path, X_OK, AT_EACCESS)) return true;
        }
        if (!dir[length]) return false;
        dir += length + 1;
    }
}

bool digby_download_available(void)
{
    char path[PATH_MAX];
    return find_curl(path);
}

static void interrupt_transfer(int signal_number)
{
    (void)signal_number;
    foreground.interrupted = 1;
}

const char *digby_download_control_begin(struct digby_download_control **out)
{
    *out = NULL;
    struct sigaction action = {0};
    action.sa_handler = interrupt_transfer;
    sigemptyset(&action.sa_mask);
    foreground.interrupted = 0;
    if (sigaction(SIGINT, &action, &foreground.previous_int)) return "cancel-handler";
    if (sigaction(SIGTERM, &action, &foreground.previous_term)) {
        sigaction(SIGINT, &foreground.previous_int, NULL);
        return "cancel-handler";
    }
    *out = &foreground;
    return NULL;
}

bool digby_download_cancelled(const struct digby_download_control *control)
{
    return control->interrupted != 0;
}

void digby_download_control_end(struct digby_download_control *control)
{
    sigaction(SIGTERM, &control->previous_term, NULL);
    sigaction(SIGINT, &control->previous_int, NULL);
}

static int start_curl(const char *executable, const char *url, const int pipe_fd[2], pid_t *pid)
{
    /* Append the final response code to stdout. --fail alone accepts 3xx.
       Reading the code separately from the body works for empty files too. */
    char *const args[] = {"curl", "--disable", "--silent", "--fail", "--globoff",
                          "--proto", "=http,https", "--max-time", "300",
                          "--write-out", "%{http_code}", "--", (char *)url, NULL};
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) return rc;
    rc = posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
    if (!rc && pipe_fd[1] != STDOUT_FILENO)
        rc = posix_spawn_file_actions_addclose(&actions, pipe_fd[1]);
    if (!rc) rc = posix_spawn(pid, executable, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc;
}

static const char *read_response(struct digby_download_control *control, int fd,
                                 digby_download_write write, void *context)
{
    enum { CHUNK = 16384, STATUS_BYTES = 3 };
    unsigned char buffer[CHUNK + STATUS_BYTES];
    size_t tail = 0;
    struct pollfd input = {fd, POLLIN, 0};
    while (!digby_download_cancelled(control)) {
        /* A signal may arrive between the flag check and a blocking read. */
        int ready = poll(&input, 1, 100);
        if (ready < 0 && errno != EINTR) return "download-failed";
        if (ready <= 0) continue;
        ssize_t count = read(fd, buffer + tail, CHUNK);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return "download-failed";
        if (!count) return tail == STATUS_BYTES && !memcmp(buffer, "200", STATUS_BYTES) ? NULL : "download-failed";
        size_t have = tail + (size_t)count;
        tail = have > STATUS_BYTES ? STATUS_BYTES : have;
        if (have > tail) {
            const char *error = write(context, buffer, have - tail);
            if (error) return error;
            memmove(buffer, buffer + have - tail, tail);
        }
    }
    return "cancelled";
}

const char *digby_download_transfer(struct digby_download_control *control, const char *url,
                                    digby_download_write write, void *context)
{
    if (digby_download_cancelled(control)) return "cancelled";
    char executable[PATH_MAX];
    if (!find_curl(executable)) return "missing-capability";
    int pipe_fd[2];
    if (pipe(pipe_fd)) return "download-failed";
    pid_t pid;
    int rc = start_curl(executable, url, pipe_fd, &pid);
    close(pipe_fd[1]);
    if (rc) {
        close(pipe_fd[0]);
        return rc == ENOENT || rc == ENOTDIR || rc == EACCES ? "missing-capability" : "download-failed";
    }
    const char *error = read_response(control, pipe_fd[0], write, context);
    close(pipe_fd[0]);
    int status;
    pid_t waited;
    do {
        if (error || digby_download_cancelled(control)) kill(pid, SIGKILL);
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (digby_download_cancelled(control)) return "cancelled";
    if (!error && (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)))
        error = "download-failed";
    return error;
}
