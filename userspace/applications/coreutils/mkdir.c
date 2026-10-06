/* mkdir [-p] DIR...: create directories (-p: with parents, existing ones are fine). */

#include <stdio.h>
#include <string.h>

#include <jelly/os.h>

static int make(const char *path, int parents)
{
    status_t status;

    if (parents) {
        char partial[1024];
        size_t length = strlen(path);
        if (length >= sizeof(partial)) {
            fprintf(stderr, "mkdir: %s: name too long\n", path);
            return 1;
        }
        for (size_t i = 1; i <= length; i++) {
            if (path[i] != '/' && path[i] != '\0')
                continue;
            memcpy(partial, path, i);
            partial[i] = '\0';
            status = jelly_mkdir(partial, 0755);
            if (STATUS_IS_ERROR(status) && status != STATUS_ALREADY_EXISTS) {
                fprintf(stderr, "mkdir: %s: %s\n", partial, strerror((int)status));
                return 1;
            }
        }
        return 0;
    }
    status = jelly_mkdir(path, 0755);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "mkdir: %s: %s\n", path, strerror((int)status));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int parents = 0, status = 0, first = 1;

    if (argc > 1 && !strcmp(argv[1], "-p")) {
        parents = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: mkdir [-p] DIR...\n");
        return 2;
    }
    for (int i = first; i < argc; i++)
        status |= make(argv[i], parents);
    return status;
}
