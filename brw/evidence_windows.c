#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include "evidence_windows.h"
#include "shim_windows.h"

enum evidence_kind { NODE, TREE, PAYLOAD };
static bool snapshot(BCRYPT_ALG_HANDLE algorithm, const wchar_t *path,
                     struct brw_evidence *result, unsigned depth, enum evidence_kind kind);

static bool children(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE hash,
                     const wchar_t *base, unsigned depth, enum evidence_kind kind)
{
    wchar_t path[BRW_PATH_CAP];
    size_t length = wcslen(base);
    if (length + 3 > BRW_PATH_CAP || depth > 16)
        return false;
    swprintf(path, BRW_PATH_CAP, L"%ls\\*", base);
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(path, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    /* Fold individually hashed (name, identity, content) pairs independent of
       enumeration order. Count also distinguishes an empty directory. */
    struct brw_evidence sum = {{0}};
    ULONG count = 0;
    bool ok = true;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
            continue;
        if (kind == PAYLOAD && !depth &&
            !(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            (!wcscmp(entry.cFileName, L".brw-files") || !wcscmp(entry.cFileName, L".brw-entry") ||
             !wcscmp(entry.cFileName, L".brw-shim") || !wcscmp(entry.cFileName, L".brw-depends")))
            continue;
        size_t chars = wcslen(entry.cFileName);
        if (length + chars + 2 > BRW_PATH_CAP) { ok = false; break; }
        wcscpy(path + length + 1, entry.cFileName);
        struct brw_evidence child, named;
        BCRYPT_HASH_HANDLE pair = NULL;
        ok = snapshot(algorithm, path, &child, depth + 1, kind) &&
             BCryptCreateHash(algorithm, &pair, NULL, 0, NULL, 0, 0) >= 0 &&
             BCryptHashData(pair, (PUCHAR)entry.cFileName, chars * sizeof(wchar_t), 0) >= 0 &&
             BCryptHashData(pair, child.bytes, sizeof child.bytes, 0) >= 0 &&
             BCryptFinishHash(pair, named.bytes, sizeof named.bytes, 0) >= 0;
        if (pair) BCryptDestroyHash(pair);
        if (!ok) break;
        for (size_t i = 0; i < sizeof sum.bytes; ++i) sum.bytes[i] ^= named.bytes[i];
        ++count;
    } while (FindNextFileW(search, &entry));
    ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return ok && BCryptHashData(hash, (PUCHAR)&count, sizeof count, 0) >= 0 &&
           BCryptHashData(hash, sum.bytes, sizeof sum.bytes, 0) >= 0;
}

static bool ordinary_streams(const wchar_t *path)
{
    WIN32_FIND_STREAM_DATA stream;
    HANDLE search = FindFirstStreamW(path, FindStreamInfoStandard, &stream, 0);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_HANDLE_EOF || error == ERROR_INVALID_FUNCTION ||
               error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER;
    }
    bool ok = true;
    do {
        if (wcscmp(stream.cStreamName, L"::$DATA")) { ok = false; break; }
    } while (FindNextStreamW(search, &stream));
    ok = ok && GetLastError() == ERROR_HANDLE_EOF;
    FindClose(search);
    return ok;
}

static bool snapshot(BCRYPT_ALG_HANDLE algorithm, const wchar_t *path,
                     struct brw_evidence *result, unsigned depth, enum evidence_kind kind)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                              FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        ZeroMemory(result, sizeof *result);
        return !depth && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
    }
    BY_HANDLE_FILE_INFORMATION info;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool ok = GetFileInformationByHandle(file, &info) &&
              !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && ordinary_streams(path) &&
              BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    if (ok) {
        DWORD identity[] = {info.dwVolumeSerialNumber, info.nFileIndexHigh,
                            info.nFileIndexLow, info.dwFileAttributes};
        DWORD directory = info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        ok = kind == PAYLOAD ? BCryptHashData(hash, (PUCHAR)&directory, sizeof directory, 0) >= 0 :
                               BCryptHashData(hash, (PUCHAR)identity, sizeof identity, 0) >= 0;
    }
    if (ok && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ok = kind == NODE || children(algorithm, hash, path, depth, kind);
    } else if (ok) {
        BYTE buffer[8192];
        DWORD size;
        do {
            ok = ReadFile(file, buffer, sizeof buffer, &size, NULL) &&
                 BCryptHashData(hash, buffer, size, 0) >= 0;
        } while (ok && size);
    }
    if (ok) ok = BCryptFinishHash(hash, result->bytes, sizeof result->bytes, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    CloseHandle(file);
    return ok;
}

static bool evidence(const wchar_t *path, struct brw_evidence *result, enum evidence_kind kind)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return false;
    bool ok = snapshot(algorithm, path, result, 0, kind);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool brw_evidence(const wchar_t *path, struct brw_evidence *result)
{
    return evidence(path, result, TREE);
}

bool brw_node_evidence(const wchar_t *path, struct brw_evidence *result)
{
    return evidence(path, result, NODE);
}

bool brw_payload_evidence(const wchar_t *path, struct brw_evidence *result)
{
    return evidence(path, result, PAYLOAD);
}
