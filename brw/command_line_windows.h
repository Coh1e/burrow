/* Encode a null-free argument vector using the Windows CRT quoting rules. */
static const char *encode_command_line(wchar_t command[32768], size_t argc,
                                       const wchar_t *const *argv)
{
    size_t used = 0;
    for (size_t i = 0; i < argc; ++i) {
        const wchar_t *p = argv[i];
        if (used + 4 >= 32768) return "child-command-too-long";
        if (i) command[used++] = L' ';
        if (*p && !wcspbrk(p, L" \t\"")) {
            size_t length = wcslen(p);
            if (used + length >= 32768) return "child-command-too-long";
            wmemcpy(command + used, p, length);
            used += length;
            continue;
        }
        command[used++] = L'"';
        for (;;) {
            size_t slashes = wcsspn(p, L"\\");
            p += slashes;
            if (!*p || *p == L'"') slashes = 2 * slashes + (*p == L'"');
            if (used + slashes + 3 > 32768) return "child-command-too-long";
            wmemset(command + used, L'\\', slashes);
            used += slashes;
            if (!*p) break;
            command[used++] = *p++;
        }
        command[used++] = L'"';
    }
    if (used >= 32767) return "child-command-too-long";
    command[used] = 0;
    return NULL;
}
