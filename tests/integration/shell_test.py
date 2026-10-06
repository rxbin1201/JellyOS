#!/usr/bin/env python3
"""Integration test for milestone M6: boot into the shell and use it.

usage: shell_test.py [--timeout SECONDS] -- QEMU COMMAND LINE...

The machine boots normally (initramfs, init, service manager, shell). The
script waits for each shell prompt on the serial console, types a command,
and checks the output that appears before the next prompt. The last command
is `poweroff`, after which QEMU must exit by itself.
Exit status 0 means every check passed.
"""

import os
import re
import select
import subprocess
import sys
import time

PROMPT = re.compile(rb"jelly:[^\r\n#]*# $")
ANSI = re.compile(rb"\x1b\[[0-9;?=]*[A-Za-z]")

# (input, strings that must appear in the output until the next prompt)
# A list of inputs is typed one after the other (for programs reading the
# console); "\x04" is Ctrl-D, every other input line gets "\r".
STEPS = [
    ("echo hello from jelly", ["hello from jelly"]),
    ("ls /bin", ["sh", "ls", "cat"]),
    ("ls -l /init", ["-rwxr-xr-x"]),
    ("echo piped text | cat | cat", ["piped text"]),
    ("echo saved > /tmp/out.txt; echo more >> /tmp/out.txt; cat < /tmp/out.txt", ["saved\nmore"]),
    ("false; echo code $?", ["code 1"]),
    ("nosuchcommand", ["nosuchcommand: command not found", "[exit 127]"]),
    ("ls /missing 2> /tmp/err; cat /tmp/err", ["ls: /missing: No such file or directory"]),
    ("cd /etc; pwd", ["/etc"]),
    ("export GREETING='hi there'; echo \"$GREETING\" ${GREETING}", ["hi there hi there"]),
    ("mkdir -p /tmp/a/b; touch /tmp/a/b/f; mv /tmp/a/b/f /tmp/a/g; ls /tmp/a; rm -r /tmp/a; ls /tmp",
     ["b/\ng", "err\nout.txt"]),
    ("svc list", ["motd", "done", "shell", "running"]),
    ("svc status nosuch", ["error: unknown service"]),
    ("cat /volumes/virtio0p1/hello.txt", ["Hello"]),
    ("cp /etc/motd /volumes/virtio0p1/motd.txt; sync", []),
    (["cat", "typed into cat", "\x04"], ["typed into cat\ntyped into cat"]),
    ("poweroff", ["Powering off."]),
]


class Console:
    def __init__(self, command, log):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
        self.buffer = b""
        self.log = log

    def read_until(self, pattern, timeout):
        """Collect output until pattern matches the end of the buffer; return the text before it."""
        deadline = time.monotonic() + timeout
        while True:
            clean = ANSI.sub(b"", self.buffer).replace(b"\r", b"")
            match = pattern.search(clean)
            if match:
                self.buffer = b""
                return clean[:match.start()].decode(errors="replace")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(clean[-400:].decode(errors="replace"))
            ready, _, _ = select.select([self.process.stdout], [], [], min(remaining, 0.5))
            if ready:
                data = os.read(self.process.stdout.fileno(), 4096)
                if not data:  # QEMU exited and everything it wrote has been read
                    raise TimeoutError(clean[-400:].decode(errors="replace"))
                self.log.write(data)
                self.buffer += data

    def send(self, text):
        data = text.encode() if text.endswith("\x04") else text.encode() + b"\r"
        # The console has no flow control: type at a human pace.
        for i in range(0, len(data), 8):
            self.process.stdin.write(data[i:i + 8])
            self.process.stdin.flush()
            time.sleep(0.02)


def main():
    args = sys.argv[1:]
    timeout = 90.0
    if args[:1] == ["--timeout"]:
        timeout = float(args[1])
        args = args[2:]
    if args[:1] != ["--"] or len(args) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    os.makedirs("build", exist_ok=True)
    log = open("build/shell-test.log", "wb")
    console = Console(args[1:], log)
    failures = 0

    try:
        console.read_until(PROMPT, timeout)
        print("shell test: shell prompt reached")
        for inputs, expected in STEPS:
            if isinstance(inputs, str):
                inputs = [inputs]
            for i, line in enumerate(inputs):
                if i:
                    time.sleep(1.0)
                console.send(line)
            command = inputs[0]
            if command == "poweroff":
                output = console.read_until(re.compile(rb"power: powering off"), timeout)
            else:
                output = console.read_until(PROMPT, timeout)
            # The first line is the console's echo of the command itself.
            output = output.split("\n", 1)[1] if "\n" in output else ""
            missing = [e for e in expected if e not in output]
            shown = " / ".join(inputs).replace("\x04", "^D")
            if missing:
                failures += 1
                print(f"shell test: FAIL  {shown!r}: missing {missing!r}\n--- output ---\n{output}\n--------------")
            else:
                print(f"shell test: ok    {shown}")
    except TimeoutError as error:
        failures += 1
        print(f"shell test: FAIL  timeout or QEMU exited early; last output:\n{error}")

    try:
        console.process.wait(timeout=30)
        print("shell test: QEMU exited after poweroff")
    except subprocess.TimeoutExpired:
        failures += 1
        print("shell test: FAIL  QEMU did not exit after poweroff")
        console.process.kill()
    log.close()
    print(f"shell test: {'PASSED' if failures == 0 else f'{failures} FAILED'} (log: build/shell-test.log)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
