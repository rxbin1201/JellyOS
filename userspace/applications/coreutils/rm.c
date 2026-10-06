/* rm [-r] [-f] PATH...: remove files (-r: directories with their contents, -f: ignore missing). */

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <jelly/os.h>

static int recursive, force;

static int remove_path(const char *path)
{
    jelly_stat_t stat;
    status_t status = jelly_stat(path, JELLY_STAT_NOFOLLOW, &stat);

    if (STATUS_IS_ERROR(status)) {
        if (force && status == STATUS_NOT_FOUND)
            return 0;
        fprintf(stderr, "rm: %s: %s\n", path, strerror((int)status));
        return 1;
    }
    if (stat.type == JELLY_FILE_TYPE_DIRECTORY) {
        if (!recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        /* Remove one entry at a time and reopen: never read a directory while changing it. */
        for (;;) {
            DIR *directory = opendir(path);
            struct dirent *d;
            char child[1024];
            int found = 0;
            if (!directory)
                break;
            while ((d = readdir(directory))) {
                if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
                    continue;
                snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
                found = 1;
                break;
            }
            closedir(directory);
            if (!found)
                break;
            if (remove_path(child))
                return 1;
        }
    }
    status = jelly_unlink(path);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "rm: %s: %s\n", path, strerror((int)status));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, status = 0;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') {
                recursive = 1;
            } else if (*o == 'f') {
                force = 1;
            } else {
                fprintf(stderr, "usage: rm [-r] [-f] PATH...\n");
                return 2;
            }
        }
    }
    if (i >= argc && !force) {
        fprintf(stderr, "usage: rm [-r] [-f] PATH...\n");
        return 2;
    }
    for (; i < argc; i++)
        status |= remove_path(argv[i]);
    return status;
}
