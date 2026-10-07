/*
 * notify TITLE [TEXT]: show a desktop notification.
 *
 * The display server passes it to the desktop shell, which shows it in the
 * top-right corner for a few seconds.
 */

#include <stdio.h>

#include "graphics/window/window.h"

int main(int argc, char **argv)
{
    wm_connection_t *connection;
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: notify TITLE [TEXT]\n");
        return 2;
    }
    if (wm_connect(&connection)) {
        fprintf(stderr, "notify: no display server\n");
        return 1;
    }
    int result = wm_notify(connection, argv[1], argc == 3 ? argv[2] : "");
    wm_disconnect(connection);
    return result ? 1 : 0;
}
