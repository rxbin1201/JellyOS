/*
 * JellyOS libc: reading directories.
 */

#ifndef _DIRENT_H
#define _DIRENT_H

#include <jelly/syscall.h>

#define DT_UNKNOWN 0
#define DT_REG     JELLY_FILE_TYPE_FILE
#define DT_DIR     JELLY_FILE_TYPE_DIRECTORY
#define DT_LNK     JELLY_FILE_TYPE_SYMLINK
#define DT_CHR     JELLY_FILE_TYPE_DEVICE
#define DT_FIFO    JELLY_FILE_TYPE_PIPE

struct dirent {
    unsigned long d_ino;
    unsigned      d_type;
    char          d_name[JELLY_NAME_MAX + 1];
};

typedef struct jelly_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int            closedir(DIR *directory);

#endif
