/*
 * JellyOS libc: starting and waiting for programs.
 *
 * There is no fork/exec: a new process is created from a program file in
 * one step (SYS_PROCESS_SPAWN) and receives explicit startup handles.
 */

#ifndef _PROCESS_H
#define _PROCESS_H

#include <jelly/syscall.h>
#include <stddef.h>

typedef struct {
    char *const   *envp;        /* NULL: the caller's environ */
    jelly_handle_t stdio[3];    /* stdin, stdout, stderr */
    jelly_handle_t extra[5];    /* further startup handles (slots 3..), JELLY_HANDLE_INVALID ends the list */
    int            as_user;     /* root only: run with uid/gid below */
    uint32_t       uid, gid;
} process_options_t;

/*
 * Start `path` with `argv` (NULL terminated). A path without '/' is searched
 * in $PATH. options NULL: inherit stdin, stdout and stderr. On success
 * *process is a handle with WAIT and MANAGE rights. Returns 0 or -1 (errno).
 */
int process_spawn(const char *path, char *const argv[], const process_options_t *options,
                  jelly_handle_t *process);

/* Wait until the process exits; *exit_code receives its code. Returns 0 or -1. */
int process_wait(jelly_handle_t process, int *exit_code);

/* Run a program to completion; returns its exit code or -1 (errno). */
int process_run(const char *path, char *const argv[], const process_options_t *options);

/* Find `name` in $PATH; writes the full path into buffer. Returns 0 or -1. */
int process_find(const char *name, char *buffer, size_t size);

#endif
