/*
 * display [MODE | off | on]: show the displays with their modes, switch
 * display 0 to another mode (root only), or switch the screen off or on.
 *
 * "off" stops the signal to the monitor, which goes to standby; pressing a
 * key or moving the mouse switches the screen on again. Root asks the
 * kernel (SYS_DISPLAY_POWER), which also works on the text console;
 * everybody else asks the display server.
 *
 * MODE is a number from the list or WIDTHxHEIGHT[@HZ]. The switch goes
 * straight to the kernel (SYS_DISPLAY_SET_MODE), so it also works on the
 * text console; a running display server notices and follows. The choice is
 * not saved: the Settings program does that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>
#include <jelly/status.h>

#include "graphics/window/window.h"

static void print_rate(uint32_t mhz)
{
    if (mhz)
        printf(" at %u.%02u Hz", mhz / 1000, mhz % 1000 / 10);
}

static void list_displays(void)
{
    jelly_display_info_t info;
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];

    for (uint32_t i = 0; jelly_display_info(i, &info) == STATUS_SUCCESS; i++) {
        uint32_t count = 0;
        printf("display %u: %ux%u", i, info.width, info.height);
        print_rate(info.refresh_mhz);
        printf("%s%s\n", (info.flags & JELLY_DISPLAY_DISCONNECTED) ? ", no monitor" : "",
               (info.flags & JELLY_DISPLAY_ACQUIRED) ? ", used by the display server" : "");
        printf("  driver:%s%s%s%s%s%s\n", (info.flags & JELLY_DISPLAY_CURSOR) ? " hardware pointer," : "",
               (info.flags & JELLY_DISPLAY_VBLANK) ? " vertical blank timing," : "",
               (info.flags & JELLY_DISPLAY_FLIP) ? " page flipping," : "",
               (info.flags & JELLY_DISPLAY_POWER) ? " screen off," : "",
               (info.flags & JELLY_DISPLAY_MODES) ? " mode switching" : " fixed mode",
               (info.flags & (JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP | JELLY_DISPLAY_MODES))
                   ? "" : " (the firmware's framebuffer)");
        if (jelly_display_modes(i, modes, JELLY_DISPLAY_MODE_MAX, &count) != STATUS_SUCCESS)
            continue;
        for (uint32_t m = 0; m < count && m < JELLY_DISPLAY_MODE_MAX; m++) {
            printf("  %2u  %ux%u", m, modes[m].width, modes[m].height);
            print_rate(modes[m].refresh_mhz);
            printf("%s%s\n", (modes[m].flags & JELLY_MODE_CURRENT) ? "  (current)" : "",
                   (modes[m].flags & JELLY_MODE_PREFERRED) ? "  (preferred)" : "");
        }
    }
}

/* The screen off or on: by the kernel if we may, else by the display server. */
static int set_power(bool on)
{
    jelly_display_info_t info;
    wm_connection_t *server;

    if (jelly_display_info(0, &info) != STATUS_SUCCESS) {
        fprintf(stderr, "display: no display\n");
        return 1;
    }
    if (!(info.flags & JELLY_DISPLAY_POWER)) {
        fprintf(stderr, "display: the graphics driver cannot switch the screen off\n");
        return 1;
    }
    status_t status = jelly_display_power(0, on);
    if (status == STATUS_ACCESS_DENIED) {
        if (wm_connect(&server) != 0) {
            fprintf(stderr, "display: only root can do that without a display server\n");
            return 1;
        }
        status = wm_set_display_power(server, on) == 0 ? STATUS_SUCCESS : STATUS_IO_ERROR;
        wm_disconnect(server);
    }
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "display: cannot switch the screen %s: %s\n", on ? "on" : "off", status_name(status));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t count = 0, width = 0, height = 0, hz = 0, mode = JELLY_DISPLAY_MODE_MAX;
    char *end;

    if (argc < 2) {
        list_displays();
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0))
        return set_power(strcmp(argv[1], "on") == 0);
    if (argc > 2 || jelly_display_modes(0, modes, JELLY_DISPLAY_MODE_MAX, &count) != STATUS_SUCCESS) {
        fprintf(stderr, "usage: display [NUMBER | WIDTHxHEIGHT[@HZ] | off | on]\n");
        return 2;
    }
    if (count > JELLY_DISPLAY_MODE_MAX)
        count = JELLY_DISPLAY_MODE_MAX;

    unsigned long first = strtoul(argv[1], &end, 10);
    if (*end == '\0' && end != argv[1]) {
        mode = first < count ? (uint32_t)first : JELLY_DISPLAY_MODE_MAX;
    } else if (*end == 'x') {
        width = (uint32_t)first;
        height = (uint32_t)strtoul(end + 1, &end, 10);
        if (*end == '@')
            hz = (uint32_t)strtoul(end + 1, &end, 10);
        for (uint32_t m = 0; m < count && mode == JELLY_DISPLAY_MODE_MAX && *end == '\0'; m++) {
            if (modes[m].width == width && modes[m].height == height && (!hz || (modes[m].refresh_mhz + 500) / 1000 == hz))
                mode = m;
        }
    }
    if (mode == JELLY_DISPLAY_MODE_MAX) {
        fprintf(stderr, "display: no mode '%s' (see the list: display)\n", argv[1]);
        return 1;
    }
    status_t status = jelly_display_set_mode(0, mode);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "display: cannot switch to %ux%u: %s\n", modes[mode].width, modes[mode].height,
                status == STATUS_NOT_SUPPORTED ? "the graphics driver cannot change the mode" : status_name(status));
        return 1;
    }
    printf("display 0: %ux%u", modes[mode].width, modes[mode].height);
    print_rate(modes[mode].refresh_mhz);
    printf("\n");
    return 0;
}
