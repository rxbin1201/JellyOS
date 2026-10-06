/*
 * Process credentials (README section 41).
 *
 * Phase 4 only records them; users, groups and permission checks on files
 * arrive with the VFS and the security model.
 */

#ifndef SECURITY_CREDENTIALS_H
#define SECURITY_CREDENTIALS_H

#include <stdint.h>

#define UID_ROOT 0
#define GID_ROOT 0

typedef struct {
    uint32_t uid;
    uint32_t gid;
} credentials_t;

#endif
