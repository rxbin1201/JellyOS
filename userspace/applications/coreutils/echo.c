/* echo [-n] [WORD...]: print the words separated by blanks. */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int newline = 1;

    if (argc > 1 && !strcmp(argv[1], "-n")) {
        newline = 0;
        argv++;
        argc--;
    }
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline)
        putchar('\n');
    return 0;
}
