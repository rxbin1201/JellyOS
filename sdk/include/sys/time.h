/*
 * JellyOS libc: struct timeval (socket timeouts).
 */

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

#endif
