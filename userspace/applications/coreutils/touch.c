/* touch FILE...: create empty files (there are no time stamps yet). */

#include <stdio.h>
#include <string.h>

#include <jelly/os.h>

int main(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: touch FILE...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        jelly_handle_t file;
        status_t s = jelly_open(argv[i], JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0644, &file);
        if (STATUS_IS_ERROR(s)) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror((int)s));
            status = 1;
            continue;
        }
        jelly_handle_close(file);
    }
    return status;
}
