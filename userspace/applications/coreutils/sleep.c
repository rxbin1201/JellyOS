/* sleep SECONDS: wait; SECONDS may have a fraction (0.5). */

#include <ctype.h>
#include <stdio.h>

#include <jelly/os.h>

int main(int argc, char **argv)
{
    uint64_t seconds = 0, ns = 0, scale = 1000000000ULL;
    const char *p;

    if (argc != 2) {
        fprintf(stderr, "usage: sleep SECONDS\n");
        return 2;
    }
    for (p = argv[1]; isdigit((unsigned char)*p); p++)
        seconds = seconds * 10 + (uint64_t)(*p - '0');
    if (*p == '.') {
        for (p++; isdigit((unsigned char)*p); p++) {
            if (scale > 1) {
                scale /= 10;
                ns += (uint64_t)(*p - '0') * scale;
            }
        }
    }
    if (*p || p == argv[1]) {
        fprintf(stderr, "sleep: invalid time '%s'\n", argv[1]);
        return 2;
    }
    jelly_thread_sleep(seconds * 1000000000ULL + ns);
    return 0;
}
