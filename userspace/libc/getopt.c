/*
 * libc: getopt (POSIX short options; "a:" takes an argument, "--" ends the options).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;

static int position = 1; /* inside argv[optind], for grouped options (-ab) */

int getopt(int argc, char *const argv[], const char *optstring)
{
    optarg = NULL;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;
    if (!strcmp(argv[optind], "--")) {
        optind++;
        return -1;
    }

    char option = argv[optind][position];
    const char *spec = option == ':' ? NULL : strchr(optstring, option);
    bool last = argv[optind][position + 1] == '\0';

    optopt = option;
    if (!spec) {
        if (opterr)
            fprintf(stderr, "%s: unknown option -%c\n", argv[0], option);
        if (last) {
            optind++;
            position = 1;
        } else {
            position++;
        }
        return '?';
    }
    if (spec[1] != ':') {
        if (last) {
            optind++;
            position = 1;
        } else {
            position++;
        }
        return option;
    }
    /* The argument follows directly (-f440) or is the next word (-f 440). */
    if (!last) {
        optarg = &argv[optind][position + 1];
        optind++;
    } else if (optind + 1 < argc) {
        optarg = argv[optind + 1];
        optind += 2;
    } else {
        optind++;
        position = 1;
        if (opterr)
            fprintf(stderr, "%s: option -%c needs an argument\n", argv[0], option);
        return optstring[0] == ':' ? ':' : '?';
    }
    position = 1;
    return option;
}
