#include "literal_offset.h"
#include <charconv>
#include <cstring>
#include <limits>

int brw_format_offset(size_t offset, char *buffer, size_t capacity)
{
    char digits[std::numeric_limits<size_t>::digits10 + 1];
    const auto result = std::to_chars(digits, digits + sizeof digits, offset);
    const size_t length = static_cast<size_t>(result.ptr - digits);
    if (!buffer || result.ec != std::errc{} || capacity <= length)
        return 1;
    std::memcpy(buffer, digits, length);
    buffer[length] = '\0';
    return 0;
}
