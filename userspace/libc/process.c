/*
 * libc: starting and waiting for programs (<process.h>).
 */

#include <errno.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static int is_executable(const char *path)
{
    jelly_stat_t stat;
    return !STATUS_IS_ERROR(jelly_stat(path, 0, &stat)) && stat.type == JELLY_FILE_TYPE_FILE &&
           (stat.mode & 0111);
}

int process_find(const char *name, char *buffer, size_t size)
{
    if (strchr(name, '/')) {
        if (strlen(name) >= size) {
            errno = ERANGE;
            return -1;
        }
        strcpy(buffer, name);
        return 0;
    }

    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    while (*path) {
        size_t length = strcspn(path, ":");
        if (length && length + 1 + strlen(name) < size) {
            memcpy(buffer, path, length);
            buffer[length] = '/';
            strcpy(buffer + length + 1, name);
            if (is_executable(buffer))
                return 0;
        }
        path += length;
        if (*path == ':')
            path++;
    }
    errno = ENOENT;
    return -1;
}

int process_spawn(const char *path, char *const argv[], const process_options_t *options,
                  jelly_handle_t *process)
{
    char resolved[1024];
    char *const *envp = options && options->envp ? options->envp : environ;
    jelly_spawn_t request = { 0 };

    if (process_find(path, resolved, sizeof(resolved)))
        return -1;

    request.path = resolved;
    request.path_length = strlen(resolved);
    request.argv = (const char *const *)argv;
    while (argv && argv[request.argc])
        request.argc++;
    request.envp = (const char *const *)envp;
    while (envp && envp[request.envc])
        request.envc++;

    for (unsigned i = 0; i < 3; i++)
        request.handles[i] = options ? options->stdio[i] : jelly_startup_handle(i);
    request.handle_count = 3;
    for (unsigned i = 0; options && i < 5 && options->extra[i] != JELLY_HANDLE_INVALID; i++)
        request.handles[request.handle_count++] = options->extra[i];

    status_t status = options && options->as_user ? jelly_spawn_as(&request, options->uid, options->gid, process)
                                                  : jelly_spawn(&request, process);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int process_wait(jelly_handle_t process, int *exit_code)
{
    jelly_process_info_t info;
    status_t status = jelly_wait(process, JELLY_WAIT_FOREVER);
    if (!STATUS_IS_ERROR(status))
        status = jelly_process_info(process, &info);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    if (exit_code)
        *exit_code = info.exit_code;
    return 0;
}

int process_run(const char *path, char *const argv[], const process_options_t *options)
{
    jelly_handle_t process;
    int code = 0;
    if (process_spawn(path, argv, options, &process))
        return -1;
    int result = process_wait(process, &code);
    jelly_handle_close(process);
    return result ? -1 : code;
}
