#ifndef BRW_SETTINGS_BACKEND_WINDOWS_H
#define BRW_SETTINGS_BACKEND_WINDOWS_H
#include <windows.h>
#include <stdbool.h>
#include "digby.h"


enum { SETTING_CAP = 32767 };
struct setting_value {
    DWORD type; /* REG_NONE denotes absence. */
    union { DWORD number; wchar_t text[SETTING_CAP]; } data;
};
struct setting_request {
    struct digby_setting setting;
    bool preview, json;
    wchar_t **child;
    int child_count;
    struct setting_value before, after;
};
#define BRW_SETTINGS_KEY L"HKCU\\Software\\Burrow\\Settings"
/* Borrows a validated operation for the duration of the call. */
const char *brw_apply_setting(void *context, const struct digby_setting *step);

#endif
