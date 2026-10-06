/*
 * JellyOS Boot Manager - ASCII string helpers.
 *
 * Configuration, command lines and boot_info strings are ASCII. UEFI APIs
 * take CHAR16; conversion happens only at that boundary.
 */

#ifndef BOOT_TEXT_H
#define BOOT_TEXT_H

#include "boot.h"

UINTN text_length(const char *s);
bool  text_equal(const char *a, const char *b);
bool  text_starts_with(const char *s, const char *prefix);

/* Copy at most capacity - 1 characters and terminate. Returns false if truncated. */
bool  text_copy(char *dest, UINTN capacity, const char *src);

/* Copy a CHAR16 string, replacing non-printable characters with '?'. */
void  text_from_wide(char *dest, UINTN capacity, const CHAR16 *src);

/* Convert "/boot/kernels/a.elf" into L"\\boot\\kernels\\a.elf". Returns false if it does not fit. */
bool  text_to_path(CHAR16 *dest, UINTN capacity, const char *path);

/* True if the space-separated command line contains exactly this token. */
bool  text_has_token(const char *cmdline, const char *token);

#endif
