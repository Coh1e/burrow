#ifndef BRW_EVIDENCE_WINDOWS_H
#define BRW_EVIDENCE_WINDOWS_H

#include <stdbool.h>
#include <wchar.h>

/* SHA-256 covers identity, type and contents, including directory children.
   A missing object has all-zero evidence. Unreadable objects return false. */
struct brw_evidence { unsigned char bytes[32]; };
bool brw_evidence(const wchar_t *path, struct brw_evidence *result);
/* Shallow directory identity remains stable as its recorded children are removed. */
bool brw_node_evidence(const wchar_t *path, struct brw_evidence *result);
/* Compare package payloads across directories, independent of file identities
   and installer metadata. Names, file kinds and every payload byte contribute. */
bool brw_payload_evidence(const wchar_t *path, struct brw_evidence *result);
#endif
