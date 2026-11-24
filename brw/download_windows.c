#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "source_windows.h"

static const char *copy_local(const wchar_t *path, FILE *output)
{
    FILE *input = _wfopen(path, L"rb");
    if (!input)
        return "download-open";
    const char *error = NULL;
    char buffer[16384];
    size_t count;
    while (!brw_cancelled() && (count = fread(buffer, 1, sizeof buffer, input)) != 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            error = "download-stage";
            break;
        }
    }
    if (ferror(input))
        error = "download-read";
    fclose(input);
    return error;
}

static const char *download_http(const wchar_t *url, FILE *output)
{
    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.dwUserNameLength ||
        parts.dwPasswordLength || !parts.dwHostNameLength ||
        parts.dwHostNameLength > 255 || wcschr(url, L'#'))
        return "download-url";

    wchar_t host[256];
    wmemcpy(host, parts.lpszHostName, parts.dwHostNameLength);
    host[parts.dwHostNameLength] = 0;
    const wchar_t *path = parts.dwUrlPathLength ? parts.lpszUrlPath : parts.lpszExtraInfo;
    wchar_t query[4096];
    if (!parts.dwUrlPathLength && parts.dwExtraInfoLength) {
        if (parts.dwExtraInfoLength + 2 > sizeof query / sizeof *query)
            return "download-url";
        query[0] = L'/';
        wcscpy(query + 1, path);
        path = query;
    }

    const char *error = "download-connect";
    HINTERNET session = WinHttpOpen(L"brw/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = NULL, request = NULL;
    if (!session || !WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000))
        goto done;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection)
        goto done;
    request = WinHttpOpenRequest(connection, L"GET", path ? path : L"/", NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
                     WINHTTP_DISABLE_AUTHENTICATION;
    if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                                      &disabled, sizeof disabled) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL))
        goto done;
    DWORD status, length = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                             WINHTTP_NO_HEADER_INDEX) || status != 200) {
        error = "download-status";
        goto done;
    }
    error = NULL;
    while (!brw_cancelled()) {
        char buffer[16384];
        DWORD count;
        if (!WinHttpReadData(request, buffer, sizeof buffer, &count)) {
            error = "download-read";
            break;
        }
        if (!count)
            break;
        if (fwrite(buffer, 1, count, output) != count) {
            error = "download-stage";
            break;
        }
    }

done:
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    if (session)
        WinHttpCloseHandle(session);
    return error;
}

bool brw_http_location(const wchar_t *location)
{
    return !_wcsnicmp(location, L"https://", 8) || !_wcsnicmp(location, L"http://", 7);
}

const char *brw_download(const wchar_t *location, FILE *destination)
{
    const char *error = brw_http_location(location)
        ? download_http(location, destination) : copy_local(location, destination);
    if (brw_cancelled())
        return "cancelled";
    if (!error && (fflush(destination) || fseek(destination, 0, SEEK_SET)))
        return "download-stage";
    return error;
}
