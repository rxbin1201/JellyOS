/* ls [-a] [-l] [PATH...]: list directory contents. */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

static int show_all, long_format;

typedef struct {
    char         name[JELLY_NAME_MAX + 1];
    jelly_stat_t stat;
} entry_t;

static char type_char(uint32_t type)
{
    switch (type) {
    case JELLY_FILE_TYPE_DIRECTORY: return 'd';
    case JELLY_FILE_TYPE_SYMLINK:   return 'l';
    case JELLY_FILE_TYPE_DEVICE:    return 'c';
    case JELLY_FILE_TYPE_PIPE:      return 'p';
    default:                        return '-';
    }
}

static void print_entry(const char *directory, const char *name, const jelly_stat_t *stat)
{
    if (!long_format) {
        printf("%s%s\n", name, stat->type == JELLY_FILE_TYPE_DIRECTORY ? "/" : "");
        return;
    }
    char mode[11];
    const char *bits = "rwxrwxrwx";
    mode[0] = type_char(stat->type);
    for (int i = 0; i < 9; i++)
        mode[1 + i] = (stat->mode & (0400u >> i)) ? bits[i] : '-';
    mode[10] = '\0';
    printf("%s %4u %4u %10lu %s", mode, stat->uid, stat->gid, (unsigned long)stat->size, name);
    if (stat->type == JELLY_FILE_TYPE_SYMLINK) {
        char path[1024], target[1024];
        size_t length;
        snprintf(path, sizeof(path), "%s/%s", directory, name);
        if (!STATUS_IS_ERROR(jelly_readlink(path, target, sizeof(target) - 1, &length))) {
            target[length] = '\0';
            printf(" -> %s", target);
        }
    }
    printf("\n");
}

static int compare_entries(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static int list(const char *path, int heading)
{
    jelly_stat_t stat;
    status_t status = jelly_stat(path, 0, &stat);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror((int)status));
        return 1;
    }
    if (stat.type != JELLY_FILE_TYPE_DIRECTORY) {
        print_entry(".", path, &stat);
        return 0;
    }

    DIR *directory = opendir(path);
    if (!directory) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    entry_t *entries = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *d;
    while ((d = readdir(directory))) {
        if (d->d_name[0] == '.' && !show_all)
            continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            entry_t *grown = realloc(entries, capacity * sizeof(entry_t));
            if (!grown) {
                fprintf(stderr, "ls: out of memory\n");
                break;
            }
            entries = grown;
        }
        entry_t *e = &entries[count++];
        char full[2048];
        snprintf(e->name, sizeof(e->name), "%s", d->d_name);
        snprintf(full, sizeof(full), "%s/%s", path, d->d_name);
        if (STATUS_IS_ERROR(jelly_stat(full, JELLY_STAT_NOFOLLOW, &e->stat))) {
            memset(&e->stat, 0, sizeof(e->stat));
            e->stat.type = d->d_type;
        }
    }
    closedir(directory);

    qsort(entries, count, sizeof(entry_t), compare_entries);
    if (heading)
        printf("%s:\n", path);
    for (size_t i = 0; i < count; i++)
        print_entry(path, entries[i].name, &entries[i].stat);
    free(entries);
    return 0;
}

int main(int argc, char **argv)
{
    int first_path = 1, status = 0;

    for (; first_path < argc && argv[first_path][0] == '-' && argv[first_path][1]; first_path++) {
        for (const char *o = argv[first_path] + 1; *o; o++) {
            if (*o == 'a') {
                show_all = 1;
            } else if (*o == 'l') {
                long_format = 1;
            } else {
                fprintf(stderr, "usage: ls [-a] [-l] [PATH...]\n");
                return 2;
            }
        }
    }
    int paths = argc - first_path;
    if (paths == 0)
        return list(".", 0);
    for (int i = first_path; i < argc; i++) {
        if (i > first_path)
            printf("\n");
        status |= list(argv[i], paths > 1);
    }
    return status;
}
