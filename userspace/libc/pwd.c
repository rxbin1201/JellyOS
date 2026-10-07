/*
 * libc: user accounts (/etc/passwd).
 */

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct passwd entry;
static char line[512];

/* Split "name:hash:uid:gid:home:shell" in place. */
static int parse(char *text)
{
    char *fields[6];
    for (int i = 0; i < 6; i++) {
        fields[i] = text;
        char *colon = strchr(text, i < 5 ? ':' : '\n');
        if (!colon) {
            if (i < 5)
                return -1;
        } else {
            *colon = '\0';
            text = colon + 1;
        }
    }
    entry.pw_name = fields[0];
    entry.pw_passwd = fields[1];
    entry.pw_uid = (uid_t)strtoul(fields[2], NULL, 10);
    entry.pw_gid = (gid_t)strtoul(fields[3], NULL, 10);
    entry.pw_dir = fields[4];
    entry.pw_shell = fields[5];
    return 0;
}

static struct passwd *find(const char *name, uid_t uid, int by_name)
{
    FILE *file = fopen("/etc/passwd", "r");
    if (!file)
        return NULL;
    struct passwd *result = NULL;
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n' || parse(line))
            continue;
        if (by_name ? !strcmp(entry.pw_name, name) : entry.pw_uid == uid) {
            result = &entry;
            break;
        }
    }
    fclose(file);
    return result;
}

struct passwd *getpwnam(const char *name)
{
    return find(name, 0, 1);
}

struct passwd *getpwuid(uid_t uid)
{
    return find(NULL, uid, 0);
}
