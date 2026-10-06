/*
 * JellyOS libc: buffered streams on JellyOS file handles.
 *
 * stdin, stdout and stderr are the program's startup handles 0, 1 and 2.
 * stdout is line buffered, stderr unbuffered.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF        (-1)
#define BUFSIZ     1024
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

typedef struct jelly_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *restrict path, const char *restrict mode);
FILE  *fdopen_handle(unsigned handle, const char *mode); /* JellyOS: wrap an open file handle */
int    fclose(FILE *stream);
int    fflush(FILE *stream);

size_t fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream);
size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream);
int    fgetc(FILE *stream);
int    getc(FILE *stream);
int    getchar(void);
int    ungetc(int c, FILE *stream);
char  *fgets(char *restrict buffer, int size, FILE *restrict stream);
int    fputc(int c, FILE *stream);
int    putc(int c, FILE *stream);
int    putchar(int c);
int    fputs(const char *restrict s, FILE *restrict stream);
int    puts(const char *s);

int    printf(const char *restrict format, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *restrict stream, const char *restrict format, ...) __attribute__((format(printf, 2, 3)));
int    sprintf(char *restrict buffer, const char *restrict format, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *restrict buffer, size_t size, const char *restrict format, ...)
           __attribute__((format(printf, 3, 4)));
int    vprintf(const char *restrict format, va_list args);
int    vfprintf(FILE *restrict stream, const char *restrict format, va_list args);
int    vsprintf(char *restrict buffer, const char *restrict format, va_list args);
int    vsnprintf(char *restrict buffer, size_t size, const char *restrict format, va_list args);

int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
void   rewind(FILE *stream);
int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
unsigned fhandle(FILE *stream); /* JellyOS: the stream's file handle */

int    remove(const char *path);
int    rename(const char *from, const char *to);
void   perror(const char *message);

#endif
