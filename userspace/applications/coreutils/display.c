/*
 * display [MODE]: show the displays with their modes, or switch display 0
 * to another mode (root only).
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
        printf("  driver:%s%s%s%s%s\n", (info.flags & JELLY_DISPLAY_CURSOR) ? " hardware pointer," : "",
               (info.flags & JELLY_DISPLAY_VBLANK) ? " vertical blank timing," : "",
               (info.flags & JELLY_DISPLAY_FLIP) ? " page flipping," : "",
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

int main(int argc, char **argv)
{
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t count = 0, width = 0, height = 0, hz = 0, mode = JELLY_DISPLAY_MODE_MAX;
    char *end;

    if (argc < 2) {
        list_displays();
        return 0;
    }
    if (argc > 2 || jelly_display_modes(0, modes, JELLY_DISPLAY_MODE_MAX, &count) != STATUS_SUCCESS) {
        fprintf(stderr, "usage: display [NUMBER | WIDTHxHEIGHT[@HZ]]\n");
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
