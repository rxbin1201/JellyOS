/*
 * JellyOS libc: user accounts from /etc/passwd.
 *
 * Format, one account per line:  name:hash:uid:gid:home:shell
 * hash is a password hash (<sha256.h>) or "!" for an account that cannot
 * log in. (JellyOS has no separate shadow file yet.)
 */

#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h>

struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwnam(const char *name); /* static result */
struct passwd *getpwuid(uid_t uid);

#endif
