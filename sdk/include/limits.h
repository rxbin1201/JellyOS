/*
 * JellyOS libc: sizes of integer types (x86_64, LP64).
 *
 * Defined here because GCC's own limits.h chains to the host C library's.
 */

#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#define MB_LEN_MAX 1
#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535
#define INT_MIN    (-INT_MAX - 1)
#define INT_MAX    __INT_MAX__
#define UINT_MAX   (INT_MAX * 2U + 1U)
#define LONG_MIN   (-LONG_MAX - 1L)
#define LONG_MAX   __LONG_MAX__
#define ULONG_MAX  (LONG_MAX * 2UL + 1UL)
#define LLONG_MIN  (-LLONG_MAX - 1LL)
#define LLONG_MAX  __LONG_LONG_MAX__
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)

#define PATH_MAX   1024
#define NAME_MAX   255

#endif
