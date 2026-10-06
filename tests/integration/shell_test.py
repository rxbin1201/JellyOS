#!/usr/bin/env python3
"""Integration test for milestones M6 and M7: boot into the shell, use it and the network.

usage: shell_test.py [--timeout SECONDS] -- QEMU COMMAND LINE...

The machine boots normally (initramfs, init, service manager, shell). The
script waits for each shell prompt on the serial console, types a command,
and checks the output that appears before the next prompt. The last command
is `poweroff`, after which QEMU must exit by itself.

For the network steps the QEMU command line must attach a NIC on the user
network. The script runs an HTTP server and TCP/UDP echo servers on the
host's 127.0.0.1, which JellyOS reaches as 10.0.2.2.
Exit status 0 means every check passed.
"""

import http.server
import os
import re
import select
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

HOST_TEXT = "Hello from the host, served over TCP to JellyOS."

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
]

FINAL_STEP = ("poweroff", ["Powering off."])


def network_steps(http_port, tcp_port, udp_port):
    """Milestone M7 over QEMU's user network (gateway 10.0.2.2 = the host's 127.0.0.1)."""
    return [
        # DHCP runs in the background (networkd); retry until the lease is there.
        ("ifconfig eth0", ["inet 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3"], 30),
        ("ping -c 2 -i 0.2 10.0.2.2", ["2 packets transmitted, 2 received"]),
        ("nslookup localhost 10.0.2.2", ["Address: 127.0.0.1", "Address: 10.0.2.2"]),
        (f"http http://10.0.2.2:{http_port}/hello.txt", [HOST_TEXT]),
        (f"http -o /tmp/page.txt http://10.0.2.2:{http_port}/hello.txt; cat /tmp/page.txt", [HOST_TEXT]),
        (f"http http://10.0.2.2:{http_port}/missing; echo code $?", ["server answered 404", "code 1"]),
        (f"echo jelly over tcp | nc 10.0.2.2 {tcp_port}", ["JELLY OVER TCP"]),
        (f"echo jelly over udp | nc -u -w 2 10.0.2.2 {udp_port}", ["JELLY OVER UDP"]),
        ("http http://10.0.2.2:1/", ["Connection refused"]),
        ("svc status network", ["network", "running"]),
    ]


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


def start_host_servers():
    """HTTP server for a temporary directory plus TCP and UDP echo servers that answer in upper case."""
    directory = tempfile.mkdtemp(prefix="jellyos-http-")
    with open(os.path.join(directory, "hello.txt"), "w") as f:
        f.write(HOST_TEXT + "\n")

    def handler(*args, **kwargs):
        return QuietHandler(*args, directory=directory, **kwargs)

    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.bind(("127.0.0.1", 0))
    tcp.listen(4)

    def tcp_echo():
        while True:
            connection, _ = tcp.accept()
            with connection:
                data = b""
                while chunk := connection.recv(4096):
                    data += chunk
                connection.sendall(data.upper())

    threading.Thread(target=tcp_echo, daemon=True).start()

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))

    def udp_echo():
        while True:
            data, sender = udp.recvfrom(65536)
            udp.sendto(data.upper(), sender)

    threading.Thread(target=udp_echo, daemon=True).start()
    return httpd.server_address[1], tcp.getsockname()[1], udp.getsockname()[1]


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
    steps = STEPS[:]
    if "virtio-net" in " ".join(args):
        steps += network_steps(*start_host_servers())
    steps.append(FINAL_STEP)
    console = Console(args[1:], log)
    failures = 0

    try:
        console.read_until(PROMPT, timeout)
        print("shell test: shell prompt reached")
        for step in steps:
            inputs, expected = step[0], step[1]
            attempts = step[2] if len(step) > 2 else 1
            if isinstance(inputs, str):
                inputs = [inputs]
            for attempt in range(attempts):
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
                if not missing:
                    break
                time.sleep(1.0)
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
