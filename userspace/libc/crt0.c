/*
 * libc program entry (README section 30).
 *
 * The kernel starts a spawned program with RDI = jelly_startup_t* (on the
 * new stack) and RSP = 8 mod 16. crt0 publishes the startup block, sets up
 * argv, environ and the standard streams, runs main and exits with its result.
 */

#include <jelly/os.h>
#include <stdlib.h>

#include "internal.h"

int main(int argc, char **argv, char **envp);

const jelly_startup_t *__jelly_startup;

__attribute__((noreturn, used)) void _start(jelly_startup_t *startup)
{
    static char *empty[1];
    int argc = 0;
    char **argv = empty;

    if (startup && startup->version == JELLY_STARTUP_VERSION) {
        __jelly_startup = startup;
        argc = (int)startup->argc;
        argv = startup->argv;
        environ = startup->envp;
    } else {
        environ = empty;
    }

    __libc_init_stdio(startup);
    exit(main(argc, argv, environ));
}

jelly_handle_t jelly_startup_handle(unsigned index)
{
    if (!__jelly_startup || index >= __jelly_startup->handle_count)
        return JELLY_HANDLE_INVALID;
    return __jelly_startup->handles[index];
}
