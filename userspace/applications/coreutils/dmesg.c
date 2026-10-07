/*
 * dmesg [WORD...]: print the kernel log (/dev/kmsg).
 *
 * With arguments only the lines containing one of the words are printed,
 * e.g. `dmesg ahci nvme`.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char line[512];
    FILE *log = fopen("/dev/kmsg", "r");

    if (!log) {
        fprintf(stderr, "dmesg: cannot open /dev/kmsg\n");
        return 1;
    }
    while (fgets(line, sizeof(line), log)) {
        int wanted = argc < 2;
        for (int i = 1; i < argc && !wanted; i++)
            wanted = strstr(line, argv[i]) != NULL;
        if (wanted)
            fputs(line, stdout);
    }
    fclose(log);
    return 0;
}
