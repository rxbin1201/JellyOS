/* mv SOURCE TARGET: rename; a directory TARGET receives SOURCE under its own name. */

#include <stdio.h>
#include <string.h>

#include <jelly/os.h>

int main(int argc, char **argv)
{
    char target[1024];
    jelly_stat_t stat;

    if (argc != 3) {
        fprintf(stderr, "usage: mv SOURCE TARGET\n");
        return 2;
    }
    snprintf(target, sizeof(target), "%s", argv[2]);
    if (!STATUS_IS_ERROR(jelly_stat(argv[2], 0, &stat)) && stat.type == JELLY_FILE_TYPE_DIRECTORY) {
        const char *base = strrchr(argv[1], '/');
        snprintf(target, sizeof(target), "%s/%s", argv[2], base ? base + 1 : argv[1]);
    }
    status_t status = jelly_rename(argv[1], target);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], target, strerror((int)status));
        return 1;
    }
    return 0;
}
