#!/usr/bin/env python3
"""Generate the system sounds of the initramfs as WAV files.

    mksound.py chime OUTPUT.wav

chime: two soft notes (E5, then A5), 22050 Hz, mono, 16 bits. The format is
on purpose not the sound card's: playing it exercises the audio server's
sample-rate and channel conversion.
"""

import math
import struct
import sys

RATE = 22050


def note(frequency, seconds, amplitude=0.45):
    count = int(RATE * seconds)
    samples = []
    for i in range(count):
        t = i / RATE
        attack = min(1.0, t / 0.01)
        decay = math.exp(-4.0 * t / seconds)
        value = math.sin(2 * math.pi * frequency * t) + 0.25 * math.sin(4 * math.pi * frequency * t)
        samples.append(amplitude * attack * decay * value / 1.25)
    return samples


def chime():
    first = note(659.26, 0.24)
    second = note(880.0, 0.46)
    return first + second


def write_wav(path, samples):
    data = b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767)))) for s in samples)
    header = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE'
    header += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b'data' + struct.pack('<I', len(data))
    with open(path, 'wb') as out:
        out.write(header + data)


def main():
    sounds = {'chime': chime}
    if len(sys.argv) != 3 or sys.argv[1] not in sounds:
        sys.exit(__doc__)
    write_wav(sys.argv[2], sounds[sys.argv[1]]())


if __name__ == '__main__':
    main()
