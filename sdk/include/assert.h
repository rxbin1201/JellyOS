/*
 * JellyOS libc: assert.
 */

#ifndef _ASSERT_H
#define _ASSERT_H

__attribute__((noreturn)) void __assert_fail(const char *expression, const char *file, int line);

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __assert_fail(#expression, __FILE__, __LINE__))
#endif

#endif
