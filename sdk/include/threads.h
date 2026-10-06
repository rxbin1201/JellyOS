/*
 * JellyOS libc: C11 threads on JellyOS threads and futexes.
 */

#ifndef _THREADS_H
#define _THREADS_H

#include <stdint.h>
#include <time.h>

enum {
    thrd_success  = 0,
    thrd_error    = 1,
    thrd_nomem    = 2,
    thrd_busy     = 3,
    thrd_timedout = 4,
};

enum {
    mtx_plain = 0,
};

typedef struct jelly_thread *thrd_t;
typedef int (*thrd_start_t)(void *);

typedef struct {
    uint32_t state; /* 0 unlocked, 1 locked, 2 locked with waiters */
} mtx_t;

int    thrd_create(thrd_t *thread, thrd_start_t function, void *arg);
int    thrd_join(thrd_t thread, int *result);
_Noreturn void thrd_exit(int result);
void   thrd_yield(void);
int    thrd_sleep(const struct timespec *duration, struct timespec *remaining);

int    mtx_init(mtx_t *mutex, int type);
int    mtx_lock(mtx_t *mutex);
int    mtx_trylock(mtx_t *mutex);
int    mtx_unlock(mtx_t *mutex);
void   mtx_destroy(mtx_t *mutex);

#endif
