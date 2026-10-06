/*
 * libc: reading directories.
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

struct jelly_dir {
    jelly_handle_t handle;
    jelly_dirent_t raw;
    struct dirent  entry;
};

DIR *opendir(const char *path)
{
    DIR *directory = malloc(sizeof(*directory));
    if (!directory) {
        errno = ENOMEM;
        return NULL;
    }
    status_t status = jelly_open(path, JELLY_OPEN_READ | JELLY_OPEN_DIRECTORY, 0, &directory->handle);
    if (STATUS_IS_ERROR(status)) {
        free(directory);
        errno = (int)status;
        return NULL;
    }
    return directory;
}

struct dirent *readdir(DIR *directory)
{
    status_t status = jelly_readdir(directory->handle, &directory->raw);
    if (status == STATUS_NOT_FOUND)
        return NULL; /* end of the directory, errno unchanged */
    if (STATUS_IS_ERROR(status)) {
        errno = (int)status;
        return NULL;
    }
    directory->entry.d_ino = directory->raw.inode;
    directory->entry.d_type = directory->raw.type;
    memcpy(directory->entry.d_name, directory->raw.name, sizeof(directory->entry.d_name));
    return &directory->entry;
}

int closedir(DIR *directory)
{
    status_t status = jelly_handle_close(directory->handle);
    free(directory);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}
