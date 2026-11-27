#include "digby_ssh.h"
#include <stdio.h>
#include <string.h>

/* Keep SSH option syntax, additional principals and ports out of the destination
   argument. The SSH client handles DNS and validates the actual host address. */
static bool valid_target(const char *target)
{
    static const char name_chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
    if (!target || *target == '-') return false;
    size_t user_length = strspn(target, name_chars);
    if (!user_length || target[user_length] != '@') return false;
    const char *host = target + user_length + 1;
    if (*host == '[') {
        const char *literal = host + 1;
        size_t length = strspn(literal, "0123456789abcdefABCDEF:.");
        return memchr(literal, ':', length) && !strcmp(literal + length, "]");
    }
    return *host && *host != '-' && host[strspn(host, name_chars)] == '\0';
}

static bool authorized(const struct digby_ssh_request *request, const char *allowed)
{
    size_t length = strlen(request->target);
    char port[sizeof ":65535"];
    snprintf(port, sizeof port, ":%u", request->port);
    return allowed && !strncmp(allowed, request->target, length) &&
           !strcmp(allowed + length, port);
}

static bool valid_file_reference(const char *path)
{
    if (!path || !*path || strstr(path, "${")) return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '"') return false;
    return true;
}

struct digby_ssh_result digby_ssh_run(const struct digby_ssh_request *request,
                                      const char *allowed_target, bool preview,
                                      const struct digby_ssh_transport *transport, void *context)
{
    struct digby_ssh_result result = {DIGBY_SSH_REJECTED, "invalid-ssh-request", -1};
    if (!request || !valid_target(request->target) || !request->port || request->port > 65535 ||
        !valid_file_reference(request->identity_file) || !valid_file_reference(request->known_hosts) ||
        !request->command || !*request->command || !request->timeout_seconds)
        return result;
    if (!authorized(request, allowed_target)) {
        result.error = "unauthorized-target";
        return result;
    }
    if (preview) {
        result.status = DIGBY_SSH_PLANNED;
        result.error = NULL;
        return result;
    }
    if (!transport || !transport->connect_verified || !transport->send_once || !transport->release) {
        result.error = "missing-capability";
        return result;
    }
    result.error = transport->connect_verified(context, request);
    if (!result.error) {
        int remote_exit = -1;
        enum digby_ssh_dispatch sent = transport->send_once(context, request, &remote_exit);
        if (sent == DIGBY_SSH_NOT_SENT) {
            result.error = "command-not-sent";
        } else if (sent == DIGBY_SSH_CONFIRMED && remote_exit >= 0 && remote_exit <= 255) {
            result.status = DIGBY_SSH_COMPLETED;
            result.remote_exit = remote_exit;
        } else {
            result.status = DIGBY_SSH_UNKNOWN;
            result.error = "remote-result-unknown";
        }
    }
    transport->release(context);
    return result;
}
