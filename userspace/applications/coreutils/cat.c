/* cat [FILE...]: copy files (or stdin, also for "-") to stdout. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int copy(FILE *in)
{
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, n, stdout) != n)
            return -1;
        /* Console input arrives line by line: show it right away. */
        if (in == stdin)
            fflush(stdout);
    }
    return ferror(in) ? -1 : 0;
}

int main(int argc, char **argv)
{
    int status = 0;

    if (argc < 2)
        return copy(stdin) ? 1 : 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            if (copy(stdin))
                status = 1;
            continue;
        }
        FILE *in = fopen(argv[i], "r");
        if (!in) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (copy(in)) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        fclose(in);
    }
    return status;
}
