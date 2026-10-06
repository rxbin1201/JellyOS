/*
 * libc: the printf family.
 *
 * Conversions: d i u o x X c s p % f F e E g G (floating point in plain
 * decimal form, enough for tools), flags - + space # 0, width and precision
 * (also *), length modifiers hh h l ll z j t L.
 */

#include <stdint.h>
#include <string.h>

#include "internal.h"

typedef struct {
    format_emit_t emit;
    void         *context;
    int           total;
} output_t;

static void out(output_t *o, const char *data, size_t length)
{
    if (length) {
        o->emit(o->context, data, length);
        o->total += (int)length;
    }
}

static void pad(output_t *o, char c, int count)
{
    char block[16];
    memset(block, c, sizeof(block));
    while (count > 0) {
        int n = count < (int)sizeof(block) ? count : (int)sizeof(block);
        out(o, block, (size_t)n);
        count -= n;
    }
}

enum {
    FLAG_LEFT  = 1 << 0,
    FLAG_PLUS  = 1 << 1,
    FLAG_SPACE = 1 << 2,
    FLAG_ALT   = 1 << 3,
    FLAG_ZERO  = 1 << 4,
};

/* Emit sign/prefix, digits and padding for an already converted number. */
static void emit_number(output_t *o, const char *prefix, const char *digits, int length, int flags, int width,
                        int precision)
{
    int prefix_length = (int)strlen(prefix);
    int zeros = precision > length ? precision - length : 0;
    int body = prefix_length + zeros + length;

    if (!(flags & FLAG_LEFT) && !(flags & FLAG_ZERO && precision < 0))
        pad(o, ' ', width - body);
    out(o, prefix, (size_t)prefix_length);
    if (!(flags & FLAG_LEFT) && (flags & FLAG_ZERO) && precision < 0)
        pad(o, '0', width - body);
    pad(o, '0', zeros);
    out(o, digits, (size_t)length);
    if (flags & FLAG_LEFT)
        pad(o, ' ', width - body);
}

static void format_integer(output_t *o, uintmax_t value, int negative, unsigned base, int upper, int flags,
                           int width, int precision, char conversion)
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[32];
    int n = sizeof(digits);
    char prefix[4] = "";

    if (!(value == 0 && precision == 0)) {
        do {
            digits[--n] = set[value % base];
            value /= base;
        } while (value);
    }

    if (negative)
        strcpy(prefix, "-");
    else if (flags & FLAG_PLUS && (conversion == 'd' || conversion == 'i'))
        strcpy(prefix, "+");
    else if (flags & FLAG_SPACE && (conversion == 'd' || conversion == 'i'))
        strcpy(prefix, " ");

    if (flags & FLAG_ALT) {
        if (base == 16 && n < (int)sizeof(digits))
            strcat(prefix, upper ? "0X" : "0x");
        else if (base == 8 && (n == (int)sizeof(digits) || digits[n] != '0'))
            digits[--n] = '0';
    }
    emit_number(o, prefix, digits + n, (int)sizeof(digits) - n, flags, width, precision);
}

static void format_float(output_t *o, long double value, int flags, int width, int precision)
{
    char buffer[64];
    char prefix[2] = "";
    int n = 0;

    if (precision < 0)
        precision = 6;
    if (precision > 18)
        precision = 18;

    if (value != value) {
        emit_number(o, "", "nan", 3, flags & ~FLAG_ZERO, width, -1);
        return;
    }
    if (value < 0) {
        prefix[0] = '-';
        value = -value;
    } else if (flags & FLAG_PLUS) {
        prefix[0] = '+';
    } else if (flags & FLAG_SPACE) {
        prefix[0] = ' ';
    }
    if (value > 1e18L) {
        emit_number(o, prefix, "inf", 3, flags & ~FLAG_ZERO, width, -1);
        return;
    }

    long double scale = 1;
    for (int i = 0; i < precision; i++)
        scale *= 10;
    long double rounded = value * scale + 0.5L;
    uint64_t whole = (uint64_t)(rounded / scale);
    uint64_t fraction = (uint64_t)(rounded - (long double)whole * scale);

    char digits[24];
    int d = 0;
    do {
        digits[d++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (d)
        buffer[n++] = digits[--d];
    if (precision > 0 || (flags & FLAG_ALT)) {
        buffer[n++] = '.';
        for (int i = precision - 1; i >= 0; i--) {
            buffer[n + i] = (char)('0' + fraction % 10);
            fraction /= 10;
        }
        n += precision;
    }
    emit_number(o, prefix, buffer, n, flags, width, -1);
}

int __libc_format(format_emit_t emit, void *context, const char *format, va_list args)
{
    output_t o = { emit, context, 0 };
    va_list ap;
    va_copy(ap, args);

    while (*format) {
        const char *start = format;
        while (*format && *format != '%')
            format++;
        out(&o, start, (size_t)(format - start));
        if (!*format)
            break;
        format++; /* '%' */

        int flags = 0;
        for (;; format++) {
            if (*format == '-') flags |= FLAG_LEFT;
            else if (*format == '+') flags |= FLAG_PLUS;
            else if (*format == ' ') flags |= FLAG_SPACE;
            else if (*format == '#') flags |= FLAG_ALT;
            else if (*format == '0') flags |= FLAG_ZERO;
            else break;
        }

        int width = 0;
        if (*format == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
            format++;
        } else {
            while (*format >= '0' && *format <= '9')
                width = width * 10 + (*format++ - '0');
        }

        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            if (*format == '*') {
                precision = va_arg(ap, int);
                format++;
            } else {
                while (*format >= '0' && *format <= '9')
                    precision = precision * 10 + (*format++ - '0');
            }
        }

        /* 0: int, 1: long, 2: long long, -1: short, -2: char, 3: size_t, 4: intmax_t, 5: ptrdiff_t */
        int size = 0, long_double = 0;
        switch (*format) {
        case 'h':
            size = -1;
            if (*++format == 'h') { size = -2; format++; }
            break;
        case 'l':
            size = 1;
            if (*++format == 'l') { size = 2; format++; }
            break;
        case 'z': size = 3; format++; break;
        case 'j': size = 4; format++; break;
        case 't': size = 5; format++; break;
        case 'L': long_double = 1; format++; break;
        }

        char conversion = *format;
        if (!conversion)
            break;
        format++;

        switch (conversion) {
        case 'd':
        case 'i': {
            intmax_t value;
            switch (size) {
            case -2: value = (signed char)va_arg(ap, int); break;
            case -1: value = (short)va_arg(ap, int); break;
            case 1:  value = va_arg(ap, long); break;
            case 2:  value = va_arg(ap, long long); break;
            case 3:  value = (intmax_t)va_arg(ap, size_t); break;
            case 4:  value = va_arg(ap, intmax_t); break;
            case 5:  value = va_arg(ap, ptrdiff_t); break;
            default: value = va_arg(ap, int); break;
            }
            uintmax_t magnitude = value < 0 ? -(uintmax_t)value : (uintmax_t)value;
            format_integer(&o, magnitude, value < 0, 10, 0, flags, width, precision, conversion);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uintmax_t value;
            switch (size) {
            case -2: value = (unsigned char)va_arg(ap, unsigned); break;
            case -1: value = (unsigned short)va_arg(ap, unsigned); break;
            case 1:  value = va_arg(ap, unsigned long); break;
            case 2:  value = va_arg(ap, unsigned long long); break;
            case 3:  value = va_arg(ap, size_t); break;
            case 4:  value = va_arg(ap, uintmax_t); break;
            case 5:  value = (uintmax_t)va_arg(ap, ptrdiff_t); break;
            default: value = va_arg(ap, unsigned); break;
            }
            unsigned base = conversion == 'u' ? 10 : conversion == 'o' ? 8 : 16;
            format_integer(&o, value, 0, base, conversion == 'X', flags, width, precision, conversion);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(ap, void *);
            format_integer(&o, value, 0, 16, 0, flags | FLAG_ALT, width, precision, 'x');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_number(&o, "", &c, 1, flags & ~FLAG_ZERO, width, -1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int length = precision >= 0 ? (int)strnlen(s, (size_t)precision) : (int)strlen(s);
            emit_number(&o, "", s, length, flags & ~FLAG_ZERO, width, -1);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            long double value = long_double ? va_arg(ap, long double) : va_arg(ap, double);
            format_float(&o, value, flags, width, precision);
            break;
        }
        case 'n':
            *va_arg(ap, int *) = o.total;
            break;
        case '%':
            out(&o, "%", 1);
            break;
        default:
            out(&o, "%", 1);
            out(&o, &conversion, 1);
            break;
        }
    }
    va_end(ap);
    return o.total;
}

/* --- Front ends ---------------------------------------------------------------- */

typedef struct {
    char  *buffer;
    size_t size;
    size_t used;
} string_sink_t;

static void emit_string(void *context, const char *data, size_t length)
{
    string_sink_t *sink = context;
    if (sink->size && sink->used < sink->size - 1) {
        size_t room = sink->size - 1 - sink->used;
        memcpy(sink->buffer + sink->used, data, length < room ? length : room);
    }
    sink->used += length;
}

int vsnprintf(char *restrict buffer, size_t size, const char *restrict format, va_list args)
{
    string_sink_t sink = { buffer, size, 0 };
    int total = __libc_format(emit_string, &sink, format, args);
    if (size)
        buffer[sink.used < size - 1 ? sink.used : size - 1] = '\0';
    return total;
}

int vsprintf(char *restrict buffer, const char *restrict format, va_list args)
{
    return vsnprintf(buffer, SIZE_MAX / 2, format, args);
}

int snprintf(char *restrict buffer, size_t size, const char *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int sprintf(char *restrict buffer, const char *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

static void emit_stream(void *context, const char *data, size_t length)
{
    fwrite(data, 1, length, context);
}

int vfprintf(FILE *restrict stream, const char *restrict format, va_list args)
{
    int total = __libc_format(emit_stream, stream, format, args);
    return ferror(stream) ? -1 : total;
}

int vprintf(const char *restrict format, va_list args)
{
    return vfprintf(stdout, format, args);
}

int fprintf(FILE *restrict stream, const char *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int printf(const char *restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}
