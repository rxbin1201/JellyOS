/*
 * libc: buffered streams on JellyOS file handles.
 *
 * A stream's buffer holds either input not yet handed out or output not
 * yet written, never both: switching direction flushes or discards it.
 * Console and pipe reads return at most one line, so a full buffer is never
 * waited for.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static unsigned char stdin_buffer[BUFSIZ];
static unsigned char stdout_buffer[BUFSIZ];

static struct jelly_file stdin_file = { .flags = FILE_READ, .buffer = stdin_buffer, .capacity = BUFSIZ,
                                        .pushback = EOF };
static struct jelly_file stdout_file = { .flags = FILE_WRITE | FILE_LINEBUF, .buffer = stdout_buffer,
                                         .capacity = BUFSIZ, .pushback = EOF };
static struct jelly_file stderr_file = { .flags = FILE_WRITE | FILE_UNBUF, .pushback = EOF };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static struct jelly_file *open_streams;

static int is_streaming(jelly_handle_t handle)
{
    jelly_stat_t stat;
    if (STATUS_IS_ERROR(jelly_fstat(handle, &stat)))
        return 1;
    return stat.type == JELLY_FILE_TYPE_DEVICE || stat.type == JELLY_FILE_TYPE_PIPE;
}

void __libc_init_stdio(const jelly_startup_t *startup)
{
    (void)startup;
    stdin_file.handle = jelly_startup_handle(JELLY_STDIN);
    stdout_file.handle = jelly_startup_handle(JELLY_STDOUT);
    stderr_file.handle = jelly_startup_handle(JELLY_STDERR);

    if (is_streaming(stdin_file.handle))
        stdin_file.flags |= FILE_STREAMING;
    /* Output into a regular file is fully buffered. */
    if (!is_streaming(stdout_file.handle))
        stdout_file.flags &= ~FILE_LINEBUF;
    stdout_file.flags |= FILE_STREAMING;
    stderr_file.flags |= FILE_STREAMING;
}

/* --- Low-level transfer ----------------------------------------------------------- */

static int write_all(FILE *stream, const unsigned char *data, size_t length)
{
    while (length) {
        size_t done = 0;
        status_t status = jelly_write(stream->handle, data, length, &done);
        if (STATUS_IS_ERROR(status) || done == 0) {
            stream->flags |= FILE_ERROR;
            errno = STATUS_IS_ERROR(status) ? (int)status : EIO;
            return EOF;
        }
        data += done;
        length -= done;
    }
    return 0;
}

static int flush_output(FILE *stream)
{
    if (!stream->writing)
        return 0;
    int result = write_all(stream, stream->buffer, stream->length);
    stream->length = 0;
    stream->writing = 0;
    return result;
}

/* Give back unread input so the handle position matches what the program consumed. */
static void drop_input(FILE *stream)
{
    if (stream->writing)
        return;
    size_t unread = stream->length - stream->position + (stream->pushback != EOF);
    if (unread && !(stream->flags & FILE_STREAMING))
        jelly_seek(stream->handle, -(int64_t)unread, JELLY_SEEK_CURRENT, NULL);
    stream->position = stream->length = 0;
    stream->pushback = EOF;
}

static int fill(FILE *stream)
{
    if (!(stream->flags & FILE_READ) || (stream->flags & (FILE_EOF | FILE_ERROR)))
        return EOF;
    if (stream->writing && flush_output(stream))
        return EOF;
    /* Prompts must be visible before the program waits for input. */
    if (stream->flags & FILE_STREAMING)
        fflush(stdout);

    size_t done = 0;
    status_t status = jelly_read(stream->handle, stream->buffer, stream->capacity, &done);
    if (STATUS_IS_ERROR(status)) {
        stream->flags |= FILE_ERROR;
        errno = (int)status;
        return EOF;
    }
    if (done == 0) {
        stream->flags |= FILE_EOF;
        return EOF;
    }
    stream->position = 0;
    stream->length = done;
    return 0;
}

static int ensure_buffer(FILE *stream)
{
    if (stream->buffer || (stream->flags & FILE_UNBUF))
        return 0;
    stream->buffer = malloc(BUFSIZ);
    if (!stream->buffer) {
        stream->flags |= FILE_UNBUF;
        return 0;
    }
    stream->capacity = BUFSIZ;
    return 0;
}

/* --- Opening and closing -------------------------------------------------------- */

static int parse_mode(const char *mode, uint32_t *open_flags, uint32_t *stream_flags)
{
    uint32_t flags;
    switch (mode[0]) {
    case 'r': flags = JELLY_OPEN_READ; *stream_flags = FILE_READ; break;
    case 'w': flags = JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_TRUNCATE; *stream_flags = FILE_WRITE; break;
    case 'a': flags = JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_APPEND; *stream_flags = FILE_WRITE; break;
    default: return -1;
    }
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') {
            flags |= JELLY_OPEN_READ | JELLY_OPEN_WRITE;
            *stream_flags = FILE_READ | FILE_WRITE;
        } else if (*m == 'x') {
            flags |= JELLY_OPEN_EXCLUSIVE;
        } else if (*m != 'b') {
            return -1;
        }
    }
    *open_flags = flags;
    return 0;
}

static FILE *new_stream(jelly_handle_t handle, uint32_t flags)
{
    FILE *stream = calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->handle = handle;
    stream->flags = flags | FILE_OWNED;
    stream->pushback = EOF;
    if (is_streaming(handle))
        stream->flags |= FILE_STREAMING;
    stream->next = open_streams;
    open_streams = stream;
    return stream;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    uint32_t open_flags, stream_flags;
    jelly_handle_t handle;

    if (!path || !mode || parse_mode(mode, &open_flags, &stream_flags)) {
        errno = EINVAL;
        return NULL;
    }
    status_t status = jelly_open(path, open_flags, 0666, &handle);
    if (STATUS_IS_ERROR(status)) {
        errno = (int)status;
        return NULL;
    }
    FILE *stream = new_stream(handle, stream_flags);
    if (!stream) {
        jelly_handle_close(handle);
        errno = ENOMEM;
    }
    return stream;
}

FILE *fdopen_handle(unsigned handle, const char *mode)
{
    uint32_t open_flags, stream_flags;
    if (!mode || parse_mode(mode, &open_flags, &stream_flags)) {
        errno = EINVAL;
        return NULL;
    }
    FILE *stream = new_stream(handle, stream_flags);
    if (!stream)
        errno = ENOMEM;
    return stream;
}

int fclose(FILE *stream)
{
    int result = fflush(stream);

    if (!(stream->flags & FILE_OWNED)) {
        /* A standard stream: close its handle, keep the object. */
        jelly_handle_close(stream->handle);
        stream->handle = JELLY_HANDLE_INVALID;
        stream->flags |= FILE_ERROR;
        return result;
    }
    for (struct jelly_file **link = &open_streams; *link; link = &(*link)->next) {
        if (*link == stream) {
            *link = stream->next;
            break;
        }
    }
    if (STATUS_IS_ERROR(jelly_handle_close(stream->handle)))
        result = EOF;
    free(stream->buffer);
    free(stream);
    return result;
}

int fflush(FILE *stream)
{
    if (!stream) {
        __libc_flush_all();
        return 0;
    }
    if (stream->writing)
        return flush_output(stream);
    drop_input(stream);
    return 0;
}

void __libc_flush_all(void)
{
    flush_output(stdout);
    flush_output(stderr);
    for (struct jelly_file *s = open_streams; s; s = s->next)
        flush_output(s);
}

/* --- Reading -------------------------------------------------------------------- */

int fgetc(FILE *stream)
{
    if (stream->pushback != EOF) {
        int c = stream->pushback;
        stream->pushback = EOF;
        return c;
    }
    if (stream->writing || stream->position >= stream->length) {
        ensure_buffer(stream);
        if (!stream->buffer) {
            unsigned char c;
            size_t done = 0;
            if (!(stream->flags & FILE_READ) || (stream->flags & (FILE_EOF | FILE_ERROR)))
                return EOF;
            status_t status = jelly_read(stream->handle, &c, 1, &done);
            if (STATUS_IS_ERROR(status) || done == 0) {
                stream->flags |= STATUS_IS_ERROR(status) ? FILE_ERROR : FILE_EOF;
                return EOF;
            }
            return c;
        }
        if (fill(stream) == EOF)
            return EOF;
    }
    return stream->buffer[stream->position++];
}

int getc(FILE *stream)
{
    return fgetc(stream);
}

int getchar(void)
{
    return fgetc(stdin);
}

int ungetc(int c, FILE *stream)
{
    if (c == EOF || stream->pushback != EOF)
        return EOF;
    stream->pushback = (unsigned char)c;
    stream->flags &= ~FILE_EOF;
    return (unsigned char)c;
}

char *fgets(char *restrict buffer, int size, FILE *restrict stream)
{
    int n = 0;
    if (size <= 0)
        return NULL;
    while (n < size - 1) {
        int c = fgetc(stream);
        if (c == EOF)
            break;
        buffer[n++] = (char)c;
        if (c == '\n')
            break;
    }
    if (n == 0)
        return NULL;
    buffer[n] = '\0';
    return buffer;
}

size_t fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
    unsigned char *out = buffer;
    size_t total = size * count, done = 0;

    if (size == 0 || count == 0)
        return 0;
    while (done < total) {
        int c = fgetc(stream);
        if (c == EOF)
            break;
        out[done++] = (unsigned char)c;
        /* Hand out buffered input in bulk. */
        size_t available = stream->length - stream->position;
        if (!stream->writing && available) {
            size_t chunk = total - done < available ? total - done : available;
            memcpy(out + done, stream->buffer + stream->position, chunk);
            stream->position += chunk;
            done += chunk;
        }
    }
    return done / size;
}

/* --- Writing -------------------------------------------------------------------- */

size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
    const unsigned char *data = buffer;
    size_t total = size * count;

    if (total == 0)
        return 0;
    if (!(stream->flags & FILE_WRITE) || (stream->flags & FILE_ERROR)) {
        errno = EBADF;
        return 0;
    }
    if (!stream->writing)
        drop_input(stream);
    ensure_buffer(stream);

    if ((stream->flags & FILE_UNBUF) || !stream->buffer) {
        if (write_all(stream, data, total))
            return 0;
        return count;
    }

    stream->writing = 1;
    size_t done = 0;
    while (done < total) {
        size_t room = stream->capacity - stream->length;
        size_t chunk = total - done < room ? total - done : room;
        memcpy(stream->buffer + stream->length, data + done, chunk);
        stream->length += chunk;
        done += chunk;
        if (stream->length == stream->capacity && flush_output(stream))
            return done / size;
        stream->writing = 1;
    }
    if ((stream->flags & FILE_LINEBUF) && memchr(data, '\n', total) && flush_output(stream))
        return 0;
    return count;
}

int fputc(int c, FILE *stream)
{
    unsigned char byte = (unsigned char)c;
    return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int putc(int c, FILE *stream)
{
    return fputc(c, stream);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *restrict s, FILE *restrict stream)
{
    size_t length = strlen(s);
    return fwrite(s, 1, length, stream) == length ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

/* --- Positioning and state ----------------------------------------------------- */

int fseek(FILE *stream, long offset, int whence)
{
    if (stream->flags & FILE_STREAMING) {
        errno = ENOSYS;
        return -1;
    }
    if (fflush(stream))
        return -1;
    status_t status = jelly_seek(stream->handle, offset, (uint32_t)whence, NULL);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    stream->flags &= ~FILE_EOF;
    return 0;
}

long ftell(FILE *stream)
{
    uint64_t position;
    if (stream->flags & FILE_STREAMING) {
        errno = ENOSYS;
        return -1;
    }
    status_t status = jelly_seek(stream->handle, 0, JELLY_SEEK_CURRENT, &position);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    if (stream->writing)
        return (long)(position + stream->length);
    return (long)(position - (stream->length - stream->position) - (stream->pushback != EOF));
}

void rewind(FILE *stream)
{
    fseek(stream, 0, SEEK_SET);
    clearerr(stream);
}

int feof(FILE *stream)
{
    return (stream->flags & FILE_EOF) != 0;
}

int ferror(FILE *stream)
{
    return (stream->flags & FILE_ERROR) != 0;
}

void clearerr(FILE *stream)
{
    stream->flags &= ~(FILE_EOF | FILE_ERROR);
}

unsigned fhandle(FILE *stream)
{
    return stream->handle;
}

/* --- Files by name -------------------------------------------------------------- */

int remove(const char *path)
{
    status_t status = jelly_unlink(path);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int rename(const char *from, const char *to)
{
    status_t status = jelly_rename(from, to);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

void perror(const char *message)
{
    if (message && *message)
        fprintf(stderr, "%s: %s\n", message, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
