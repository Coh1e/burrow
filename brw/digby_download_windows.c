#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "digby_download.h"
#include "source_windows.h"

struct digby_download_control { volatile LONG interrupted; };
static struct digby_download_control foreground;

bool digby_download_available(void)
{
    return true; /* WinHTTP is linked into this build. */
}

static BOOL WINAPI interrupt_transfer(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&foreground.interrupted, 1);
    return TRUE;
}

const char *digby_download_control_begin(struct digby_download_control **out)
{
    *out = NULL;
    InterlockedExchange(&foreground.interrupted, 0);
    if (!SetConsoleCtrlHandler(interrupt_transfer, TRUE)) return "cancel-handler";
    *out = &foreground;
    return NULL;
}

bool digby_download_cancelled(const struct digby_download_control *control)
{
    /* The foreground controller has process lifetime, including during delivery
       of a console event on a Windows handler thread. */
    (void)control;
    return InterlockedCompareExchange(&foreground.interrupted, 0, 0) != 0;
}

void digby_download_control_end(struct digby_download_control *control)
{
    (void)control;
    SetConsoleCtrlHandler(interrupt_transfer, FALSE);
}

struct completion {
    HANDLE ready, closed;
    DWORD status, bytes;
};

static void CALLBACK notify(HINTERNET request, DWORD_PTR context, DWORD status,
                            void *data, DWORD length)
{
    (void)request;
    (void)data;
    struct completion *c = (struct completion *)context;
    if (!c) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        SetEvent(c->closed);
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        c->status = status;
        c->bytes = length;
        SetEvent(c->ready);
    }
}

static bool completed(struct completion *c, BOOL started, DWORD expected, bool (*cancelled)(void))
{
    while (started && !cancelled()) {
        DWORD r = WaitForSingleObject(c->ready, 100);
        if (r != WAIT_TIMEOUT) return r == WAIT_OBJECT_0 && c->status == expected;
    }
    return false;
}

/* Close-delimited and chunked replies have no required Content-Length.
   When the server supplies it, a short clean close is still an error. */
static bool content_length(HINTERNET request, uint64_t *size, bool *known)
{
    wchar_t value[32];
    DWORD length = sizeof value;
    *size = 0;
    *known = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                 value, &length, WINHTTP_NO_HEADER_INDEX) != 0;
    if (!*known) return GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND;
    if (!value[0]) return false;
    for (const wchar_t *p = value; *p; ++p) {
        if (*p < L'0' || *p > L'9') return false;
        unsigned digit = (unsigned)(*p - L'0');
        if (*size > (UINT64_MAX - digit) / 10) return false;
        *size = *size * 10 + digit;
    }
    return true;
}

const char *brw_download_http(const wchar_t *url, digby_download_write write, void *context,
                              bool (*cancelled)(void))
{
    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength ||
        !parts.dwHostNameLength || parts.dwHostNameLength > 255 || wcschr(url, L'#') ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        return "download-url";

    wchar_t host[256];
    wmemcpy(host, parts.lpszHostName, parts.dwHostNameLength);
    host[parts.dwHostNameLength] = 0;
    const wchar_t *path = parts.dwUrlPathLength ? parts.lpszUrlPath : parts.lpszExtraInfo;
    wchar_t query[4096];
    if (!parts.dwUrlPathLength && parts.dwExtraInfoLength) {
        if (parts.dwExtraInfoLength + 2 > sizeof query / sizeof *query) return "download-url";
        query[0] = L'/';
        wcscpy(query + 1, path);
        path = query;
    }

    const char *error = "download-connect";
    HINTERNET session = WinHttpOpen(L"brw/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    HINTERNET connection = NULL, request = NULL;
    struct completion c = {0};
    bool subscribed = false;
    /* WinHTTP may use this buffer until the final handle-closing callback. */
    unsigned char buffer[16384];
    if (!session || !WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000)) goto done;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path ? path : L"/", NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled))
        goto done;
    c.ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    c.closed = CreateEventW(NULL, FALSE, FALSE, NULL);
    DWORD_PTR binding = (DWORD_PTR)&c;
    DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
                          WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                          WINHTTP_CALLBACK_FLAG_HANDLES;
    if (!c.ready || !c.closed ||
        !WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &binding, sizeof binding) ||
        WinHttpSetStatusCallback(request, notify, notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        goto done;
    subscribed = true;
    if (cancelled() ||
        !completed(&c, WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, binding),
                   WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancelled) ||
        !completed(&c, WinHttpReceiveResponse(request, NULL), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancelled))
        goto done;
    DWORD status, length = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        error = "download-status";
        goto done;
    }
    uint64_t promised, received = 0;
    bool known;
    error = "download-read";
    if (!content_length(request, &promised, &known)) goto done;
    while (completed(&c, WinHttpReadData(request, buffer, sizeof buffer, NULL),
                     WINHTTP_CALLBACK_STATUS_READ_COMPLETE, cancelled)) {
        if (!c.bytes) {
            if (!known || received == promised) error = NULL;
            break;
        }
        received += c.bytes;
        if (known && received > promised) break;
        const char *failure = write(context, buffer, c.bytes);
        if (failure) { error = failure; break; }
    }

done:
    if (request) WinHttpCloseHandle(request);
    /* Closing the request cancels any outstanding async I/O. The closing
       callback is the last user of both the buffer and its completion context. */
    if (subscribed) WaitForSingleObject(c.closed, INFINITE);
    if (c.ready) CloseHandle(c.ready);
    if (c.closed) CloseHandle(c.closed);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return cancelled() ? "cancelled" : error;
}

static bool foreground_cancelled(void)
{
    return digby_download_cancelled(&foreground);
}

const char *digby_download_transfer(struct digby_download_control *control, const char *url,
                                    digby_download_write write, void *context)
{
    if (digby_download_cancelled(control)) return "cancelled";
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, NULL, 0);
    if (!size) return "download-url";
    wchar_t *wide = malloc((size_t)size * sizeof *wide);
    if (!wide) return "out-of-memory";
    const char *error = "download-url";
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wide, size))
        error = brw_download_http(wide, write, context, foreground_cancelled);
    free(wide);
    if (error && (!strcmp(error, "download-connect") || !strcmp(error, "download-status") ||
                  !strcmp(error, "download-read"))) return "download-failed";
    return error;
}
