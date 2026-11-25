#ifndef BRW_GIT_WINDOWS_H
#define BRW_GIT_WINDOWS_H
#include <stdbool.h>
#include <wchar.h>
#define BRW_GIT_USAGE "brw git clone SOURCE DIRECTORY | brw git pull DIRECTORY [--dry-run] [--json]"
int brw_git_command(size_t count, const wchar_t **args, bool json, bool preview);
#endif
