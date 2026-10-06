/*
 * http [-v] [-o FILE] URL: fetch a URL with HTTP/1.0 GET.
 *
 * Only http:// (there is no TLS yet). The body goes to stdout or FILE;
 * -v prints the status line and headers to stderr. Up to 5 redirects
 * (301, 302, 303, 307, 308) are followed. Exit code 0 for a 2xx answer.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_REDIRECTS 5
#define HEADER_MAX    16384

typedef struct {
    char host[256];
    int  port;
    char path[1024];
} url_t;

static int verbose;

static int parse_url(const char *text, url_t *url)
{
    if (!strncmp(text, "https://", 8)) {
        fprintf(stderr, "http: https is not supported (no TLS yet)\n");
        return -1;
    }
    if (!strncmp(text, "http://", 7))
        text += 7;
    size_t host_length = strcspn(text, ":/");
    if (host_length == 0 || host_length >= sizeof(url->host)) {
        fprintf(stderr, "http: bad URL\n");
        return -1;
    }
    memcpy(url->host, text, host_length);
    url->host[host_length] = '\0';
    text += host_length;
    url->port = 80;
    if (*text == ':') {
        char *end;
        url->port = (int)strtol(text + 1, &end, 10);
        if (url->port <= 0 || url->port > 65535 || (*end && *end != '/')) {
            fprintf(stderr, "http: bad port\n");
            return -1;
        }
        text = end;
    }
    snprintf(url->path, sizeof(url->path), "%s", *text ? text : "/");
    return 0;
}

static int connect_to(const url_t *url)
{
    struct hostent *entry = gethostbyname(url->host);
    if (!entry) {
        fprintf(stderr, "http: %s: %s\n", url->host, strerror(errno));
        return -1;
    }
    struct sockaddr_in address = { AF_INET, htons((uint16_t)url->port), { 0 }, { 0 } };
    memcpy(&address.sin_addr, entry->h_addr, 4);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = { 30, 0 };
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (verbose)
        fprintf(stderr, "* connecting to %s (%s) port %d\n", url->host, inet_ntoa(address.sin_addr), url->port);
    if (connect(s, (struct sockaddr *)&address, sizeof(address))) {
        fprintf(stderr, "http: connect to %s:%d: %s\n", url->host, url->port, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

static int send_all(int s, const char *data, size_t length)
{
    while (length) {
        ssize_t n = send(s, data, length, 0);
        if (n <= 0)
            return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

/* Find a header value (case-insensitive name) in the header block. */
static int header_value(const char *headers, const char *name, char *value, size_t size)
{
    size_t length = strlen(name);
    for (const char *line = headers; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL) {
        size_t i = 0;
        while (i < length && line[i] && (line[i] | 0x20) == (name[i] | 0x20))
            i++;
        if (i == length && line[i] == ':') {
            const char *v = line + i + 1;
            while (*v == ' ')
                v++;
            size_t n = strcspn(v, "\r\n");
            if (n >= size)
                n = size - 1;
            memcpy(value, v, n);
            value[n] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Fetch once. Returns the status code (body written to out) or -1; *location set for redirects. */
static int fetch(const url_t *url, FILE *out, char *location, size_t location_size)
{
    static char header[HEADER_MAX + 1];
    char request[1536];
    int s = connect_to(url);
    if (s < 0)
        return -1;

    int length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: JellyOS-http/0.8\r\nAccept: */*\r\n"
                          "Connection: close\r\n\r\n",
                          url->path, url->host);
    if (send_all(s, request, (size_t)length)) {
        fprintf(stderr, "http: send: %s\n", strerror(errno));
        close(s);
        return -1;
    }

    /* Read until the end of the headers. */
    size_t used = 0;
    char *body = NULL;
    while (!body) {
        if (used == HEADER_MAX) {
            fprintf(stderr, "http: headers too long\n");
            close(s);
            return -1;
        }
        ssize_t n = recv(s, header + used, HEADER_MAX - used, 0);
        if (n <= 0) {
            fprintf(stderr, "http: %s\n", n < 0 ? strerror(errno) : "connection closed before the headers ended");
            close(s);
            return -1;
        }
        used += (size_t)n;
        header[used] = '\0';
        body = strstr(header, "\r\n\r\n");
    }
    body += 4;
    size_t body_bytes = used - (size_t)(body - header);
    body[-2] = '\0'; /* end the header block after its last line */

    /* Status line: "HTTP/1.x NNN reason" */
    int code = 0;
    if (!strncmp(header, "HTTP/", 5) && strchr(header, ' '))
        code = atoi(strchr(header, ' ') + 1);
    if (code < 100 || code > 999) {
        fprintf(stderr, "http: not an HTTP response\n");
        close(s);
        return -1;
    }
    if (verbose)
        fprintf(stderr, "%s\n", header);
    location[0] = '\0';
    header_value(strstr(header, "\r\n") ? strstr(header, "\r\n") + 2 : "", "Location", location, location_size);

    size_t total = 0;
    int redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
    if (!redirect && body_bytes) {
        fwrite(body, 1, body_bytes, out);
        total += body_bytes;
    }
    char buffer[4096];
    for (;;) {
        ssize_t n = recv(s, buffer, sizeof(buffer), 0);
        if (n < 0) {
            fprintf(stderr, "http: %s\n", strerror(errno));
            break;
        }
        if (n == 0)
            break;
        if (!redirect)
            fwrite(buffer, 1, (size_t)n, out);
        total += (size_t)n;
    }
    close(s);
    if (verbose)
        fprintf(stderr, "* %lu bytes of body\n", (unsigned long)total);
    return code;
}

int main(int argc, char **argv)
{
    const char *output = NULL, *target = NULL;
    char location[1024];
    url_t url;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            output = argv[++i];
        else if (!target && argv[i][0] != '-')
            target = argv[i];
        else
            target = NULL, i = argc;
    }
    if (!target) {
        fprintf(stderr, "usage: http [-v] [-o FILE] URL\n");
        return 2;
    }
    if (parse_url(target, &url))
        return 2;

    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out) {
        fprintf(stderr, "http: %s: %s\n", output, strerror(errno));
        return 1;
    }
    int code = -1;
    for (int redirects = 0; redirects <= MAX_REDIRECTS; redirects++) {
        code = fetch(&url, out, location, sizeof(location));
        if (code < 300 || code >= 400 || !location[0])
            break;
        if (location[0] == '/') {
            snprintf(url.path, sizeof(url.path), "%s", location);
        } else if (parse_url(location, &url)) {
            code = -1;
            break;
        }
        if (verbose)
            fprintf(stderr, "* redirected to %s\n", location);
    }
    if (out != stdout)
        fclose(out);
    else
        fflush(stdout);
    if (code >= 300)
        fprintf(stderr, "http: server answered %d\n", code);
    return code >= 200 && code < 300 ? 0 : 1;
}
