/*
 * Starting programs from files (README section 28, Phase 7: ELF userspace loader).
 */

#ifndef PROCESS_SPAWN_H
#define PROCESS_SPAWN_H

#include "process/process.h"

#include <jelly/syscall.h>

#define SPAWN_MAX_IMAGE (64ULL << 20)

typedef struct {
    const char          *path;          /* absolute */
    char *const         *argv;          /* kernel copies */
    uint32_t             argc;
    char *const         *envp;
    uint32_t             envc;
    object_t            *objects[JELLY_SPAWN_MAX_HANDLES];
    uint32_t             rights[JELLY_SPAWN_MAX_HANDLES];
    uint32_t             handle_count;
    const credentials_t *credentials;
    const char          *cwd;
} spawn_request_t;

/*
 * Load path (needs execute permission), install the startup handles, build
 * the jelly_startup_t block on the new stack and start the main thread with
 * RDI pointing at it. The caller receives a reference to the process.
 */
status_t process_spawn(const spawn_request_t *request, process_t **process);

#endif
