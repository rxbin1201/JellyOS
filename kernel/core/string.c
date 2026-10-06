/*
 * Freestanding memory routines. The compiler may emit calls to these.
 */

#include "core/string.h"

#include <stdint.h>

void *memset(void *dest, int value, size_t count)
{
    uint8_t *d = dest;
    while (count--)
        *d++ = (uint8_t)value;
    return dest;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (count--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    uint8_t *d = dest;
    const uint8_t *s = src;

    if (d < s) {
        while (count--)
            *d++ = *s++;
    } else {
        while (count--)
            d[count] = s[count];
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const uint8_t *x = a, *y = b;
    for (; count; count--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t count)
{
    for (; count; count--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}
