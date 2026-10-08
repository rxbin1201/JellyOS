/*
 * volume: show or change the system volume.
 *
 *   volume              show volume, mute state and the sound devices
 *   volume PERCENT      set the volume (0-100)
 *   volume +N | -N      change it by N
 *   volume mute | unmute
 *   volume output N     let the sound go out on device N of the list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/client/audio.h"

int main(int argc, char **argv)
{
    uint32_t percent;
    bool muted;
    audio_info_t info;

    status_t status = audio_get_master(&percent, &muted);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "volume: no audio server (%s)\n", status_name(status));
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "output") && argv[2][0] >= '0' && argv[2][0] <= '9') {
        audio_device_entry_t device;
        uint32_t index = (uint32_t)atoi(argv[2]);
        status = audio_set_output(index);
        if (STATUS_IS_ERROR(status) || STATUS_IS_ERROR(audio_get_device(index, &device))) {
            fprintf(stderr, "volume: device %u cannot be the output: %s\n", index, status_name(status));
            return 1;
        }
        printf("volume: output %u: %s\n", index, device.name);
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: volume [percent | +n | -n | mute | unmute | output n]\n");
        return 2;
    }
    if (argc == 2) {
        const char *arg = argv[1];
        if (!strcmp(arg, "mute")) {
            muted = true;
        } else if (!strcmp(arg, "unmute")) {
            muted = false;
        } else if ((arg[0] == '+' || arg[0] == '-') && arg[1] >= '0' && arg[1] <= '9') {
            int value = (int)percent + atoi(arg);
            percent = value < 0 ? 0 : value > 100 ? 100 : (uint32_t)value;
        } else if (arg[0] >= '0' && arg[0] <= '9') {
            int value = atoi(arg);
            percent = value > 100 ? 100 : (uint32_t)value;
        } else {
            fprintf(stderr, "usage: volume [percent | +n | -n | mute | unmute | output n]\n");
            return 2;
        }
        status = audio_set_master(percent, muted);
        if (STATUS_IS_ERROR(status)) {
            fprintf(stderr, "volume: %s\n", status_name(status));
            return 1;
        }
    }
    printf("volume: %u%%%s\n", percent, muted ? " (muted)" : "");
    if (argc == 1 && !STATUS_IS_ERROR(audio_get_info(&info))) {
        audio_device_entry_t device;
        printf("volume: %s, %u Hz, %u channels, %u stream%s, %u underruns\n", info.name, info.rate, info.channels,
               info.streams, info.streams == 1 ? "" : "s", info.underruns);
        for (uint32_t i = 0; !STATUS_IS_ERROR(audio_get_device(i, &device)); i++)
            printf("  %u  %s (%s%s%s)%s%s\n", i, device.name, (device.directions & AUDIO_PLAYBACK) ? "plays" : "",
                   (device.directions & AUDIO_PLAYBACK) && (device.directions & AUDIO_CAPTURE) ? ", " : "",
                   (device.directions & AUDIO_CAPTURE) ? "records" : "", device.output ? "  <- output" : "",
                   device.input ? "  <- input" : "");
    }
    return 0;
}
