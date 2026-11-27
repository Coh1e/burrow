#ifndef BRW_SSH_WINDOWS_H
#define BRW_SSH_WINDOWS_H

#include "digby_ssh.h"

/* Uses Windows OpenSSH and streams remote output to the caller's stderr. */
struct digby_ssh_result brw_windows_ssh_run(const struct digby_ssh_request *request,
                                           const char *allowed_target, bool preview);

#endif
