#!/usr/bin/env python3
"""Send commands to a QEMU monitor (HMP) on a UNIX socket.

usage: qemu_monitor.py SOCKET COMMAND...

Each argument is one monitor command, for example
    qemu_monitor.py build/monitor.sock "screendump build/shot.ppm" quit
"""

import socket
import sys
import time


def run(path, commands):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(50):
        try:
            s.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(5)
    output = b""

    def read_prompt():
        nonlocal output
        while not output.endswith(b"(qemu) "):
            data = s.recv(4096)
            if not data:
                return
            output += data

    read_prompt()
    for command in commands:
        output = b""
        s.sendall(command.encode() + b"\n")
        if command == "quit":
            break
        read_prompt()
    s.close()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2])
    run(sys.argv[1], sys.argv[2:])
