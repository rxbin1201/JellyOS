/*
 * libc: strings and memory.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memset(void *dest, int value, size_t count)
{
    unsigned char *d = dest;
    while (count--)
        *d++ = (unsigned char)value;
    return dest;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (count--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

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
    const unsigned char *x = a, *y = b;
    for (; count; count--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t count)
{
    const unsigned char *p = s;
    for (; count; count--, p++) {
        if (*p == (unsigned char)c)
            return (void *)p;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
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

char *strcpy(char *restrict dest, const char *restrict src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *restrict dest, const char *restrict src, size_t count)
{
    size_t i = 0;
    for (; i < count && src[i]; i++)
        dest[i] = src[i];
    for (; i < count; i++)
        dest[i] = '\0';
    return dest;
}

char *strcat(char *restrict dest, const char *restrict src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strncat(char *restrict dest, const char *restrict src, size_t count)
{
    char *d = dest + strlen(dest);
    while (count-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    for (;; s++) {
        if (*s == (char)c)
            found = s;
        if (!*s)
            return (char *)found;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t length = strlen(needle);
    if (!length)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle && !strncmp(haystack, needle, length))
            return (char *)haystack;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *restrict s, const char *restrict delimiters, char **restrict state)
{
    if (!s)
        s = *state;
    s += strspn(s, delimiters);
    if (!*s) {
        *state = s;
        return NULL;
    }
    char *end = s + strcspn(s, delimiters);
    if (*end)
        *end++ = '\0';
    *state = end;
    return s;
}

char *strtok(char *restrict s, const char *restrict delimiters)
{
    static char *state;
    return strtok_r(s, delimiters, &state);
}

char *strdup(const char *s)
{
    size_t length = strlen(s) + 1;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, s, length);
    return copy;
}

char *strndup(const char *s, size_t max)
{
    size_t length = strnlen(s, max);
    char *copy = malloc(length + 1);
    if (copy) {
        memcpy(copy, s, length);
        copy[length] = '\0';
    }
    return copy;
}

char *strerror(int error)
{
    switch (error) {
    case 0:         return "Success";
    case EINVAL:    return "Invalid argument";
    case ENOENT:    return "No such file or directory";
    case EACCES:    return "Permission denied";
    case ENOMEM:    return "Out of memory";
    case EBUSY:     return "Resource busy";
    case ENOSYS:    return "Not supported";
    case EIO:       return "I/O error";
    case ETIMEDOUT: return "Timed out";
    case STATUS_DEVICE_ERROR: return "Device error";
    case EAGAIN:    return "Would block";
    case ERANGE:    return "Buffer too small";
    case EPIPE:     return "Peer closed";
    case EBADF:     return "Bad handle";
    case EMFILE:    return "Limit exceeded";
    case EINTR:     return "Interrupted";
    case EEXIST:    return "Already exists";
    case ENOTDIR:   return "Not a directory";
    case EISDIR:    return "Is a directory";
    case ENOTEMPTY: return "Directory not empty";
    case ENOSPC:    return "No space left";
    default:        return "Unknown error";
    }
}
