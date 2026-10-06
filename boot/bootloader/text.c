#include "text.h"

UINTN text_length(const char *s)
{
    UINTN n = 0;
    while (s[n])
        n++;
    return n;
}

bool text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

bool text_starts_with(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

bool text_copy(char *dest, UINTN capacity, const char *src)
{
    UINTN i = 0;

    for (; src[i] && i + 1 < capacity; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return src[i] == '\0';
}

void text_from_wide(char *dest, UINTN capacity, const CHAR16 *src)
{
    UINTN i = 0;

    for (; src && src[i] && i + 1 < capacity; i++)
        dest[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    dest[i] = '\0';
}

bool text_to_path(CHAR16 *dest, UINTN capacity, const char *path)
{
    UINTN i = 0;

    for (; path[i]; i++) {
        if (i + 1 >= capacity)
            return false;
        dest[i] = path[i] == '/' ? L'\\' : (CHAR16)path[i];
    }
    dest[i] = L'\0';
    return true;
}

bool text_has_token(const char *cmdline, const char *token)
{
    UINTN length = text_length(token);
    const char *p = cmdline;

    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        if ((UINTN)(p - start) == length && CompareMem(start, token, length) == 0)
            return true;
    }
    return false;
}
