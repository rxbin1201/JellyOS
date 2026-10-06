/*
 * libc: process exit, environment, number conversion, sorting, errno.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int errno;
char **environ;

int __libc_fail(status_t status)
{
    errno = (int)status;
    return -1;
}

/* --- Exit ----------------------------------------------------------------------- */

#define ATEXIT_MAX 32
static void (*exit_handlers[ATEXIT_MAX])(void);
static int exit_handler_count;

int atexit(void (*function)(void))
{
    if (exit_handler_count == ATEXIT_MAX)
        return -1;
    exit_handlers[exit_handler_count++] = function;
    return 0;
}

void exit(int status)
{
    while (exit_handler_count)
        exit_handlers[--exit_handler_count]();
    __libc_flush_all();
    jelly_process_exit(status);
}

void _Exit(int status)
{
    jelly_process_exit(status);
}

/* There are no signals: abort ends the process with the code a POSIX shell
   reports for SIGABRT. */
void abort(void)
{
    __libc_flush_all();
    jelly_process_exit(134);
}

void __assert_fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expression, file, line);
    abort();
}

/* --- Environment ----------------------------------------------------------------- */

/* environ starts as the startup block's array; the first change copies it to the heap. */
static int environ_owned;
static size_t environ_capacity;

static size_t environ_count(void)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    return n;
}

static char **find_variable(const char *name, size_t length)
{
    for (char **e = environ; e && *e; e++) {
        if (!strncmp(*e, name, length) && (*e)[length] == '=')
            return e;
    }
    return NULL;
}

char *getenv(const char *name)
{
    char **entry = find_variable(name, strlen(name));
    return entry ? *entry + strlen(name) + 1 : NULL;
}

static int own_environ(size_t needed)
{
    size_t count = environ_count();
    if (environ_owned && count + needed + 1 <= environ_capacity)
        return 0;
    size_t capacity = (count + needed + 1) * 2;
    char **copy = malloc(capacity * sizeof(char *));
    if (!copy)
        return -1;
    for (size_t i = 0; i < count; i++)
        copy[i] = environ[i];
    copy[count] = NULL;
    if (environ_owned)
        free(environ);
    environ = copy;
    environ_owned = 1;
    environ_capacity = capacity;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t length = strlen(name);
    if (!length || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    char **entry = find_variable(name, length);
    if (entry && !overwrite)
        return 0;

    char *text = malloc(length + strlen(value) + 2);
    if (!text || own_environ(1)) {
        free(text);
        errno = ENOMEM;
        return -1;
    }
    strcpy(text, name);
    text[length] = '=';
    strcpy(text + length + 1, value);

    /* Replaced strings may live in the startup block, so they are not freed. */
    entry = find_variable(name, length);
    if (entry) {
        *entry = text;
    } else {
        size_t count = environ_count();
        environ[count] = text;
        environ[count + 1] = NULL;
    }
    return 0;
}

int unsetenv(const char *name)
{
    char **entry = find_variable(name, strlen(name));
    if (!entry)
        return 0;
    do {
        entry[0] = entry[1];
    } while (*entry++);
    return 0;
}

/* --- Numbers -------------------------------------------------------------------- */

static unsigned long long parse_unsigned(const char *s, char **end, int base, int *negative, int *overflow)
{
    const char *p = s;
    unsigned long long value = 0;
    int digits = 0;

    while (isspace((unsigned char)*p))
        p++;
    *negative = 0;
    if (*p == '+' || *p == '-')
        *negative = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }

    *overflow = 0;
    for (;; p++) {
        int digit;
        if (isdigit((unsigned char)*p))
            digit = *p - '0';
        else if (isalpha((unsigned char)*p))
            digit = tolower((unsigned char)*p) - 'a' + 10;
        else
            break;
        if (digit >= base)
            break;
        if (value > (ULLONG_MAX - (unsigned)digit) / (unsigned)base)
            *overflow = 1;
        value = value * (unsigned)base + (unsigned)digit;
        digits++;
    }
    if (end)
        *end = (char *)(digits ? p : s);
    return value;
}

unsigned long long strtoull(const char *restrict s, char **restrict end, int base)
{
    int negative, overflow;
    unsigned long long value = parse_unsigned(s, end, base, &negative, &overflow);
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? -value : value;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base)
{
    return strtoull(s, end, base);
}

long long strtoll(const char *restrict s, char **restrict end, int base)
{
    int negative, overflow;
    unsigned long long value = parse_unsigned(s, end, base, &negative, &overflow);
    if (overflow || value > (unsigned long long)LLONG_MAX + negative) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    return negative ? (long long)-value : (long long)value;
}

long strtol(const char *restrict s, char **restrict end, int base)
{
    return strtoll(s, end, base);
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

long long atoll(const char *s)
{
    return strtoll(s, NULL, 10);
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

div_t div(int numerator, int denominator)
{
    div_t result = { numerator / denominator, numerator % denominator };
    return result;
}

static unsigned long long random_state = 1;

int rand(void)
{
    random_state = random_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(random_state >> 33);
}

void srand(unsigned seed)
{
    random_state = seed;
}

/* --- Sorting and searching ------------------------------------------------------ */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Heap sort: no recursion and O(n log n) in every case. */
static void sift_down(unsigned char *base, size_t root, size_t count, size_t size,
                      int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= count)
            return;
        if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(base + root * size, base + child * size) >= 0)
            return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    unsigned char *b = base;
    if (count < 2 || size == 0)
        return;
    for (size_t i = count / 2; i-- > 0;)
        sift_down(b, i, count, size, compare);
    for (size_t end = count - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift_down(b, 0, end, size, compare);
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const unsigned char *b = base;
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = compare(key, b + middle * size);
        if (order == 0)
            return (void *)(b + middle * size);
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return NULL;
}
