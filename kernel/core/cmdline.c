#include "core/cmdline.h"

#include "core/string.h"

#define CMDLINE_MAX 1024

static char cmdline[CMDLINE_MAX];

void cmdline_init(const char *source)
{
    size_t i = 0;

    for (; source && source[i] && i + 1 < CMDLINE_MAX; i++)
        cmdline[i] = source[i];
    cmdline[i] = '\0';
}

const char *cmdline_get(void)
{
    return cmdline;
}

bool cmdline_value(const char *key, char *value, size_t size)
{
    size_t key_length = strlen(key);
    const char *p = cmdline;

    while (*p) {
        while (*p == ' ')
            p++;
        const char *token = p;
        while (*p && *p != ' ')
            p++;

        if ((size_t)(p - token) > key_length && strncmp(token, key, key_length) == 0 &&
            token[key_length] == '=') {
            const char *v = token + key_length + 1;
            size_t n = 0;
            for (; v + n < p && n + 1 < size; n++)
                value[n] = v[n];
            if (size)
                value[n] = '\0';
            return true;
        }
    }
    return false;
}
