/*
 * Kernel tests: formatting, strings, command line.
 */

#include "tests/kernel/ktest.h"

#include "core/format.h"
#include "core/string.h"

#include <stdint.h>

#define EXPECT_FORMAT(expected, ...)                         \
    do {                                                     \
        char buf[64];                                        \
        format(buf, sizeof(buf), __VA_ARGS__);               \
        KEXPECT(strcmp(buf, expected) == 0);                 \
    } while (0)

KTEST(format_integers)
{
    EXPECT_FORMAT("42", "%d", 42);
    EXPECT_FORMAT("-42", "%d", -42);
    EXPECT_FORMAT("-0042", "%05d", -42);
    EXPECT_FORMAT("00007", "%05u", 7u);
    EXPECT_FORMAT("ff FF", "%x %X", 255u, 255u);
    EXPECT_FORMAT("18446744073709551615", "%lu", UINT64_MAX);
    EXPECT_FORMAT("-9223372036854775808", "%ld", INT64_MIN);
    EXPECT_FORMAT("0x00000000deadbeef", "%p", (void *)0xdeadbeef);
}

KTEST(format_strings_and_padding)
{
    EXPECT_FORMAT("[ab  ]", "[%-4s]", "ab");
    EXPECT_FORMAT("[  ab]", "[%4s]", "ab");
    const char *volatile none = 0; /* hide the NULL from the compiler's format check */
    EXPECT_FORMAT("x(null)", "x%s", none);
    EXPECT_FORMAT("100%", "%d%%", 100);
    EXPECT_FORMAT("c", "%c", 'c');
}

KTEST(format_truncation)
{
    char buf[5];
    size_t length = format(buf, sizeof(buf), "%s", "jellyfish");
    KEXPECT(length == 9);
    KEXPECT(strcmp(buf, "jell") == 0);
}

KTEST(string_compare)
{
    KEXPECT(strcmp("a", "a") == 0);
    KEXPECT(strcmp("a", "b") < 0);
    KEXPECT(strncmp("jelly", "jellyfish", 5) == 0);
    KEXPECT(strlen("jelly") == 5);
}
