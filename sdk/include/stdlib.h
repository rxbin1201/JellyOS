/*
 * JellyOS libc: general utilities.
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF

typedef struct { int quot, rem; } div_t;

void  *malloc(size_t size);
void  *calloc(size_t count, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
__attribute__((noreturn)) void abort(void);
int    atexit(void (*function)(void));

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);
long   strtol(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end, int base);

int    abs(int value);
long   labs(long value);
div_t  div(int numerator, int denominator);

int    rand(void);
void   srand(unsigned seed);

void   qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*compare)(const void *, const void *));

char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);

extern char **environ;

#endif
