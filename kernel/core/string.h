/*
 * Freestanding string and memory routines (kernel/core/string.c).
 */

#ifndef CORE_STRING_H
#define CORE_STRING_H

#include <stddef.h>

void  *memset(void *dest, int value, size_t count);
void  *memcpy(void *restrict dest, const void *restrict src, size_t count);
void  *memmove(void *dest, const void *src, size_t count);
int    memcmp(const void *a, const void *b, size_t count);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t count);

#endif
