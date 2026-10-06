/*
 * libc internals shared between the source files (not part of the SDK).
 */

#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H

#include <jelly/os.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#define FILE_READ      (1u << 0) /* opened for reading */
#define FILE_WRITE     (1u << 1) /* opened for writing */
#define FILE_EOF       (1u << 2)
#define FILE_ERROR     (1u << 3)
#define FILE_OWNED     (1u << 4) /* fclose closes the handle and frees the object */
#define FILE_LINEBUF   (1u << 5) /* flush on '\n' */
#define FILE_UNBUF     (1u << 6) /* every write goes straight to the handle */
#define FILE_STREAMING (1u << 7) /* console or pipe: no seeking */

struct jelly_file {
    jelly_handle_t     handle;
    uint32_t           flags;
    unsigned char     *buffer;
    size_t             capacity;
    size_t             position;  /* reading: next byte to hand out */
    size_t             length;    /* reading: bytes in the buffer; writing: bytes pending */
    int                writing;   /* the buffer holds pending output */
    int                pushback;  /* ungetc character, or EOF */
    struct jelly_file *next;      /* list of fopen'd streams */
};

void __libc_init_stdio(const jelly_startup_t *startup);
void __libc_flush_all(void);

/* Set errno from a status code and return -1. */
int __libc_fail(status_t status);

/* printf engine: emit() receives each piece of output; returns the total length. */
typedef void (*format_emit_t)(void *context, const char *data, size_t length);
int __libc_format(format_emit_t emit, void *context, const char *format, va_list args);

#endif
