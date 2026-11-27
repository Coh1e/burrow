#ifndef BRW_DIGBY_SSH_H
#define BRW_DIGBY_SSH_H

#include <stdbool.h>

/* All strings are borrowed, NUL-terminated UTF-8 for the duration of the call.
   target is user@host (use brackets for IPv6); identity_file and known_hosts
   name operator-supplied files. File references cannot contain controls, '"'
   or the OpenSSH environment expansion introducer "${". timeout_seconds
   bounds the whole operation. */
struct digby_ssh_request {
    const char *target, *identity_file, *known_hosts, *command;
    unsigned port, timeout_seconds;
};

enum digby_ssh_dispatch {
    DIGBY_SSH_NOT_SENT, DIGBY_SSH_UNCONFIRMED, DIGBY_SSH_CONFIRMED
};

struct digby_ssh_transport {
    /* Opens one session, verifying the pinned host before returning NULL.
       On failure returns a static error code. Never sends the requested command. */
    const char *(*connect_verified)(void *context, const struct digby_ssh_request *request);
    /* Uses that same session. Once any command bytes may have been sent, an
       unconfirmed exit is UNCONFIRMED. Only CONFIRMED sets remote_exit (0..255).
       The backend reports command output independently of this result. */
    enum digby_ssh_dispatch (*send_once)(void *context, const struct digby_ssh_request *request,
                                         int *remote_exit);
    /* Called once after every attempted connect, including partial failures. */
    void (*release)(void *context);
};

enum digby_ssh_status {
    DIGBY_SSH_REJECTED, DIGBY_SSH_PLANNED, DIGBY_SSH_COMPLETED, DIGBY_SSH_UNKNOWN
};

struct digby_ssh_result {
    enum digby_ssh_status status;
    const char *error; /* Static error code, or NULL for planned/completed. */
    int remote_exit;   /* -1 unless completed (including a known nonzero exit). */
};

/* allowed_target is a separate operator authorization, exactly user@host:port,
   with a decimal port and brackets for an IPv6 host. Preview runs the same
   request checks as execution, and permits a NULL transport. */
struct digby_ssh_result digby_ssh_run(const struct digby_ssh_request *request,
                                      const char *allowed_target, bool preview,
                                      const struct digby_ssh_transport *transport, void *context);

#endif
