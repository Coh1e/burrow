#ifndef BRW_CLEANUP_WINDOWS_H
#define BRW_CLEANUP_WINDOWS_H
#include <stdio.h>
#include <stdbool.h>
#include <wchar.h>

/* The caller owns the open journal and the ordered array of private roots.
   Writes a bounded-path, children-first manifest at the current file offset. */
bool brw_record_cleanup(FILE *journal, const wchar_t *roots[], unsigned count);
/* Reads that manifest twice, checking every remaining object before removal.
   The caller retires the journal on success. Errors are static strings. */
const char *brw_run_cleanup(FILE *journal, const wchar_t *roots[], unsigned count, bool preview);
#endif
