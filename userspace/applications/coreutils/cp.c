/* cp SOURCE TARGET: copy a file; a directory TARGET receives SOURCE under its own name. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <jelly/os.h>

int main(int argc, char **argv)
{
    char target[1024], buffer[8192];
    jelly_stat_t stat;
    size_t n;
    int status = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: cp SOURCE TARGET\n");
        return 2;
    }
    snprintf(target, sizeof(target), "%s", argv[2]);
    if (!STATUS_IS_ERROR(jelly_stat(argv[2], 0, &stat)) && stat.type == JELLY_FILE_TYPE_DIRECTORY) {
        const char *base = strrchr(argv[1], '/');
        snprintf(target, sizeof(target), "%s/%s", argv[2], base ? base + 1 : argv[1]);
    }

    FILE *in = fopen(argv[1], "r");
    if (!in) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    FILE *out = fopen(target, "w");
    if (!out) {
        fprintf(stderr, "cp: %s: %s\n", target, strerror(errno));
        fclose(in);
        return 1;
    }
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, out) != n) {
            fprintf(stderr, "cp: %s: %s\n", target, strerror(errno));
            status = 1;
            break;
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        status = 1;
    }
    fclose(in);
    if (fclose(out)) {
        fprintf(stderr, "cp: %s: %s\n", target, strerror(errno));
        status = 1;
    }
    return status;
}
