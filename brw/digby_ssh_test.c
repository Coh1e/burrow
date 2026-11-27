#include "digby_ssh.h"
#include <stdio.h>
#include <string.h>

struct simulation {
    const char *connect_error;
    enum digby_ssh_dispatch dispatch;
    int exit_code;
    unsigned connections, attempts, sends, releases;
    const struct digby_ssh_request *verified;
    bool invalid_order;
};

static const char *connect_verified(void *context, const struct digby_ssh_request *request)
{
    struct simulation *sim = context;
    ++sim->connections;
    if (!sim->connect_error)
        sim->verified = request;
    return sim->connect_error;
}

static enum digby_ssh_dispatch send_once(void *context, const struct digby_ssh_request *request,
                                         int *remote_exit)
{
    struct simulation *sim = context;
    ++sim->attempts;
    if (sim->dispatch != DIGBY_SSH_NOT_SENT)
        ++sim->sends;
    if (sim->verified != request || sim->releases)
        sim->invalid_order = true;
    if (sim->dispatch == DIGBY_SSH_CONFIRMED)
        *remote_exit = sim->exit_code;
    return sim->dispatch;
}

static void release(void *context)
{
    struct simulation *sim = context;
    ++sim->releases;
    sim->verified = NULL;
}

static const struct digby_ssh_transport backend = {connect_verified, send_once, release};
static const struct digby_ssh_request valid = {
    "builder@host", "identity", "known hosts", "test -d build", 2222, 10
};
static const char allowed[] = "builder@host:2222";

struct scenario {
    const char *name, *authorization;
    bool preview;
    const char *connect_error;
    enum digby_ssh_dispatch dispatch;
    int exit_code;
    enum digby_ssh_status expected;
    const char *error;
    unsigned connections, sends;
};

static bool check(const struct scenario *test)
{
    struct simulation sim = {0};
    sim.connect_error = test->connect_error;
    sim.dispatch = test->dispatch;
    sim.exit_code = test->exit_code;
    struct digby_ssh_result result = digby_ssh_run(&valid, test->authorization,
                                                    test->preview, &backend, &sim);
    int expected_exit = test->expected == DIGBY_SSH_COMPLETED ? test->exit_code : -1;
    bool same_error = test->error && result.error ? !strcmp(test->error, result.error)
                                                   : test->error == result.error;
    unsigned expected_attempts = test->connect_error ? 0 : test->connections;
    if (result.status == test->expected && result.remote_exit == expected_exit && same_error &&
        sim.connections == test->connections && sim.sends == test->sends &&
        sim.attempts == expected_attempts && sim.releases == sim.connections && !sim.invalid_order)
        return true;
    fprintf(stderr, "%s: status=%d exit=%d error=%s connects=%u attempts=%u sends=%u releases=%u\n",
            test->name, (int)result.status, result.remote_exit, result.error ? result.error : "-",
            sim.connections, sim.attempts, sim.sends, sim.releases);
    return false;
}

static bool invalid_request(const struct digby_ssh_request *request)
{
    struct simulation sim = {0};
    struct digby_ssh_result result = digby_ssh_run(request, allowed, false, &backend, &sim);
    return result.status == DIGBY_SSH_REJECTED && result.remote_exit == -1 && result.error &&
           !strcmp(result.error, "invalid-ssh-request") && !sim.connections && !sim.sends &&
           !sim.releases;
}

int main(void)
{
    const struct scenario cases[] = {
        {"preview", allowed, true, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_PLANNED, NULL, 0, 0},
        {"absent authorization", NULL, false, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "unauthorized-target", 0, 0},
        {"different login", "another@host:2222", false, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "unauthorized-target", 0, 0},
        {"different peer", "builder@host.example:2222", false, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "unauthorized-target", 0, 0},
        {"different port", "builder@host:22", false, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "unauthorized-target", 0, 0},
        {"wrong host key", allowed, false, "host-identity", DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "host-identity", 1, 0},
        {"disconnected after send", allowed, false, NULL, DIGBY_SSH_UNCONFIRMED, 0,
         DIGBY_SSH_UNKNOWN, "remote-result-unknown", 1, 1},
        {"send failed before any bytes", allowed, false, NULL, DIGBY_SSH_NOT_SENT, 0,
         DIGBY_SSH_REJECTED, "command-not-sent", 1, 0},
        {"confirmed success", allowed, false, NULL, DIGBY_SSH_CONFIRMED, 0,
         DIGBY_SSH_COMPLETED, NULL, 1, 1},
        {"confirmed remote 255", allowed, false, NULL, DIGBY_SSH_CONFIRMED, 255,
         DIGBY_SSH_COMPLETED, NULL, 1, 1},
        {"malformed confirmation", allowed, false, NULL, DIGBY_SSH_CONFIRMED, 256,
         DIGBY_SSH_UNKNOWN, "remote-result-unknown", 1, 1}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i)
        if (!check(&cases[i])) return 1;

    struct digby_ssh_request request = valid;
    if (!invalid_request(NULL)) return 2;
    request.port = 65536;
    if (!invalid_request(&request)) return 3;
    request.port = valid.port;
    request.timeout_seconds = 0;
    if (!invalid_request(&request)) return 4;
    request = valid;
    request.command = "";
    if (!invalid_request(&request)) return 5;
    request = valid;
    request.known_hosts = NULL;
    if (!invalid_request(&request)) return 6;
    request = valid;
    request.identity_file = "";
    if (!invalid_request(&request)) return 7;
    request.identity_file = "${other}/id";
    if (!invalid_request(&request)) return 7;
    request = valid;
    request.known_hosts = "line\nother";
    if (!invalid_request(&request)) return 7;
    request.known_hosts = "\"pins\"";
    if (!invalid_request(&request)) return 7;
    request = valid;
    const char *bad_targets[] = {NULL, "", "-o@peer", "@host", "user@", "user@host:22",
                                 "dup@user@host", "user@has spaces", "user@[::1"};
    for (size_t i = 0; i < sizeof bad_targets / sizeof *bad_targets; ++i) {
        request.target = bad_targets[i];
        if (!invalid_request(&request)) return 8;
    }
    const struct {
        const char *target;
        bool valid;
    } ipv6[] = {
        {"builder@[::1]", true}, {"builder@[::]", true}, {"builder@[2001:db8::]", true},
        {"builder@[1:2:3:4:5:6:7:8]", true}, {"builder@[::ffff:192.0.2.1]", true},
        {"builder@[1:2:3:4:5:6:192.0.2.1]", true},
        {"builder@[:::]", false}, {"builder@[1::2::3]", false}, {"builder@[1:2:3]", false},
        {"builder@[1:2:3:4:5:6:7:8:9]", false}, {"builder@[::1:2:3:4:5:6:7:8]", false},
        {"builder@[12345::]", false}, {"builder@[1:2:3:4:5:6:7:]", false},
        {"builder@[::ffff:192.0.2.256]", false}, {"builder@[::ffff:192.0.2]", false},
        {"builder@[::ffff:192.00.2.1]", false}, {"builder@[192.0.2.1::]", false},
        {"builder@[::1:2:3:4:5:6:192.0.2.1]", false}, {"builder@[::1]suffix", false}
    };
    for (size_t i = 0; i < sizeof ipv6 / sizeof *ipv6; ++i) {
        request.target = ipv6[i].target;
        char authorization[80];
        snprintf(authorization, sizeof authorization, "%s:2222", request.target);
        struct digby_ssh_result result = digby_ssh_run(&request, authorization, true, NULL, NULL);
        enum digby_ssh_status expected = ipv6[i].valid ? DIGBY_SSH_PLANNED : DIGBY_SSH_REJECTED;
        if (result.status != expected ||
            (!ipv6[i].valid && (!result.error || strcmp(result.error, "invalid-ssh-request")))) {
            fprintf(stderr, "unexpected IPv6 preview for %s: status=%d\n", request.target, (int)result.status);
            return 9;
        }
    }
    if (digby_ssh_run(&valid, allowed, false, NULL, NULL).status != DIGBY_SSH_REJECTED)
        return 10;
    struct digby_ssh_transport partial = backend;
    partial.release = NULL;
    struct simulation sim = {0};
    if (digby_ssh_run(&valid, allowed, false, &partial, &sim).status != DIGBY_SSH_REJECTED ||
        sim.connections || sim.sends)
        return 11;
    return 0;
}
