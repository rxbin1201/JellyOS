#include "core/format.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char  *buffer;
    size_t size;
    size_t length;
} output_t;

static void put(output_t *out, char c)
{
    if (out->length + 1 < out->size)
        out->buffer[out->length] = c;
    out->length++;
}

static void put_padded(output_t *out, const char *s, size_t length, unsigned width, bool left, char pad)
{
    size_t fill = width > length ? width - length : 0;

    if (!left) {
        while (fill--)
            put(out, pad);
    }
    while (length--)
        put(out, *s++);
    if (left) {
        while (fill--)
            put(out, ' ');
    }
}

static void put_number(output_t *out, uint64_t value, unsigned base, bool upper, bool negative,
                       unsigned width, bool left, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    size_t n = 0;

    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value);

    char text[25];
    size_t length = 0;

    /* With zero padding the sign goes before the zeros. */
    if (negative && pad == '0') {
        put(out, '-');
        if (width)
            width--;
    } else if (negative) {
        text[length++] = '-';
    }
    while (n)
        text[length++] = tmp[--n];
    put_padded(out, text, length, width, left, pad);
}

size_t format_v(char *buffer, size_t size, const char *fmt, va_list args)
{
    output_t out = { buffer, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&out, *fmt);
            continue;
        }
        fmt++;

        bool left = false;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        if (left)
            pad = ' ';

        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (unsigned)(*fmt++ - '0');

        int longs = 0;
        bool size_type = false;
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            size_type = true;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s)
                s = "(null)";
            size_t length = 0;
            while (s[length])
                length++;
            put_padded(&out, s, length, width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            put_padded(&out, &c, 1, width, left, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = size_type ? (int64_t)va_arg(args, long) :
                        longs ? (int64_t)va_arg(args, long long) : (int64_t)va_arg(args, int);
            uint64_t magnitude = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            put_number(&out, magnitude, 10, false, v < 0, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = size_type ? (uint64_t)va_arg(args, size_t) :
                         longs ? (uint64_t)va_arg(args, unsigned long long) : (uint64_t)va_arg(args, unsigned);
            put_number(&out, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, left, pad);
            break;
        }
        case 'p':
            put(&out, '0');
            put(&out, 'x');
            put_number(&out, (uint64_t)(uintptr_t)va_arg(args, void *), 16, false, false, 16, false, '0');
            break;
        case '%':
            put(&out, '%');
            break;
        case '\0':
            fmt--; /* stray '%' at the end */
            break;
        default:
            put(&out, '%');
            put(&out, *fmt);
            break;
        }
    }

    if (size)
        buffer[out.length < size ? out.length : size - 1] = '\0';
    return out.length;
}

size_t format(char *buffer, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    size_t length = format_v(buffer, size, fmt, args);
    va_end(args);
    return length;
}
