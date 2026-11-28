#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

static const char *write_to_file(void *context, const unsigned char *bytes, size_t size)
{
    return fwrite(bytes, 1, size, context) == size ? NULL : "download-stage";
}

bool brw_http_location(const wchar_t *location)
{
    return !_wcsnicmp(location, L"https://", 8) || !_wcsnicmp(location, L"http://", 7);
}

const char *brw_download(const wchar_t *location, FILE *destination)
{
    const char *error = brw_http_location(location)
        ? brw_download_http(location, write_to_file, destination, brw_cancelled) : copy_local(location, destination);
    if (brw_cancelled())
        return "cancelled";
    if (!error && (fflush(destination) || fseek(destination, 0, SEEK_SET)))
        return "download-stage";
    return error;
}
