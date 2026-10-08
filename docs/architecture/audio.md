# JellyOS Audio

**Code:** [`drivers/audio/`](../../drivers/audio/) (Intel HDA), [`audio/device/`](../../audio/device/) (kernel: audio devices), [`audio/mixer/`](../../audio/mixer/), [`audio/client/`](../../audio/client/) (audio API), [`userspace/services/audio/audiod.c`](../../userspace/services/audio/audiod.c) (audio server), [`userspace/applications/audio/`](../../userspace/applications/audio/) (play, record, tone, volume)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 7) · **Tests:** [../development/testing.md](../development/testing.md)

Phase 11 (milestone M10): JellyOS plays and records sound. README section 38:

```text
Audio Driver       drivers/audio/intel_hda.c        the sound card
     ↓
Audio Device       audio/device/                    ring buffers, system calls 71-75
     ↓
Audio Server       /sbin/audiod, service "audio"    owns the card, one shared ring per stream
     ↓
Mixer              audio/mixer/                     sample rates, volumes, sum of all streams
     ↓
Application        audio/client/audio.h             audio_open, audio_write, audio_read, ...
```

## Audio devices (kernel)

A driver registers an `audio_device_t`: name, directions, format (interleaved
signed 16-bit frames; the HDA driver uses 48 000 Hz stereo), period (frames
the hardware takes at a time) and two operations to start and stop each
direction. The device layer puts a ring of 8 periods between the driver and
the user:

- **Playback:** the user queues frames with `SYS_AUDIO_WRITE`; the driver
  calls `audio_playback_pull()` for each period from its interrupt handler.
  When the ring runs dry the rest of the period is silence and counts as an
  underrun.
- **Capture:** the driver calls `audio_capture_push()`; the user reads with
  `SYS_AUDIO_READ`. What is not read in time is dropped, oldest first
  (overrun).

One user at a time opens a device (`SYS_AUDIO_OPEN`, root only: the audio
server). The handle is waitable: it is signaled while playback runs and at
most two periods are queued, or while a period of recorded frames waits. The
server therefore sleeps between periods and the **sound card sets the pace**:
no timers, no drift between a software clock and the hardware. Closing the
handle stops both directions.

## Intel HD Audio driver

`drivers/audio/intel_hda.c` binds to PCI class 04.03.

- **Controller:** reset, CORB/RIRB rings for codec commands (responses are
  polled; each one is acknowledged in RIRBSTS), one output and one input
  stream descriptor, MSI interrupt.
- **Codecs:** the driver reads every codec's widget tree and searches paths
  along the connection lists (through mixers and selectors):
  output pin (line out, speaker, headphones) → DAC, and ADC → input pin
  (microphone, line in). It selects the connections, opens the amplifiers
  at 0 dB, enables the pins and binds the converters to the streams. Each
  stream goes to one codec: all outputs of the first codec that has any, and
  the first input found. Volume is left to the mixer.
- **Streams:** a cyclic DMA buffer of 8 fragments of 10 ms with an interrupt
  after each. Playback keeps the fragment after the one being played filled;
  the position register (LPIB) tells how far the hardware is, so a late
  interrupt skips what it missed instead of playing stale data.

Tested with QEMU's codecs (hda-duplex, hda-output, hda-micro). Not yet: jack
detection, digital outputs (HDMI/DisplayPort), other formats, suspend.

## Audio server and mixer

`/sbin/audiod` (service `audio` in `/etc/services.conf`) owns the sound
devices and registers the service "audio". Without a sound card it exits
and `audio_open()` reports NOT_FOUND.

A machine can have several sound devices: the sound card's jacks, the
loudspeakers of a monitor. The server uses two of them, which may be the
same: the **output**, where everything that is played goes, and the
**input**, where recordings come from. At the start the output is the first
device that can play (or the one `/etc/audio.conf` names with `output=N`)
and the input the first that can record. The output can be changed while
sound plays (`AUDIO_SET_OUTPUT`): the old card is stopped, the streams'
converters are set to the new card's rate, and they play on there; what was
queued in the old card, a fraction of a second, is lost. The input stays
where it is, so recording keeps working when the sound goes out on a device
that cannot record.

Every stream is a **shared-memory ring** in the client's own format
([`protocol.h`](../../audio/client/protocol.h)): the client fills it (playback)
or empties it (capture); only control messages use the channel. Whoever finds
the ring full or empty sleeps on a futex on the other side's counter. The
server keeps its own copy of each ring's size and format and never trusts
the header a client can write.

When the device handle signals, the server

- **plays:** takes what each active stream has, converts it to 48 000 Hz
  stereo (linear interpolation, mono duplicated), scales it by the stream
  volume, sums all streams in 32 bits, applies the master volume and limits
  the result to 16 bits (clipping instead of wrap-around), and queues it so
  that 4 periods (40 ms) are buffered;
- **records:** converts what the card recorded to each capture stream's
  format.

The card only runs while needed: a stream that stays empty for about 300 ms
goes idle (the client library restarts it with `AUDIO_START` on the next
write); output stops 200 ms after the last stream went idle; recording stops
when the last capture stream closes. `audio_drain()` returns when the
stream's last frame has left the card.

Volume in percent maps to a quadratic gain (50 % = a quarter of the
amplitude, -12 dB). `/etc/audio.conf` sets the start values (`volume=`,
`muted=`, `output=`).

Latency from `audio_write()` to the speaker: 40 ms device ring plus 10-20 ms
in the card's buffer, plus whatever the client keeps in its own ring (up to
about a quarter of a second if it writes ahead).

## Audio API

[`audio/client/audio.h`](../../audio/client/audio.h), in `libaudio.a` (linked
into every program):

| Function | |
| --- | --- |
| `audio_open(direction, rate, channels, &stream)` | `AUDIO_PLAYBACK` or `AUDIO_CAPTURE`; 4 000-192 000 Hz, mono or stereo |
| `audio_write(stream, frames, count)` | blocks while the ring is full |
| `audio_read(stream, frames, count)` | blocks until the frames are recorded |
| `audio_available(stream)` | frames that can be written/read without blocking |
| `audio_drain(stream)` | wait until everything written was played |
| `audio_set_volume(stream, percent)` | volume of this stream |
| `audio_get_master` / `audio_set_master` | system volume and mute |
| `audio_get_device(index, &entry)` / `audio_set_output(index)` | the sound devices (name, plays or records, whether it is the output or the input), and which one the sound goes out on |
| `audio_get_info(&info)` | sound card, open streams, underruns |
| `audio_close(stream)` | |

## Programs

| Program | |
| --- | --- |
| `tone [-f Hz] [-d ms] [-v percent] [-r rate] [-m]` | sine tone (libc got a small `<math.h>` for it) |
| `play [-v percent] file.wav...` | PCM WAV files, 8/16 bits, mono/stereo, any rate |
| `record [-d seconds] [-r rate] [-c channels] file.wav` | record to a WAV file |
| `volume [percent \| +n \| -n \| mute \| unmute]` | system volume; without arguments also the output's card, statistics and the list of sound devices |
| `volume output N` | let the sound go out on device N of that list |
| Settings → Sound | volume, mute, test sound (`/usr/share/sounds/chime.wav`, generated by `tools/image_builder/mksound.py`), and the output device if there is more than one |

`make run AUDIO=pa` (or `alsa`, `sdl`, ...) connects QEMU's sound card to the
host; the default `AUDIO=none` is silent.

## Open points

- No access control: every program may play and record.
- Fixed at 48 000 Hz stereo. The input cannot be chosen (it is the first device that records), and the chosen
  output is not saved across restarts.
- Linear interpolation is audible on demanding material; a better resampler
  belongs to the audio libraries of Phase 13.
- The master volume is not saved across restarts.
- No low-latency path (README "later"), no Bluetooth audio.
