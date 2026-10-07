#!/usr/bin/env python3
"""Integration test for milestones M6, M7 and M8: the shell, the network and graphical applications.

usage: shell_test.py [--timeout SECONDS] [--qmp SOCKET] -- QEMU COMMAND LINE...

The machine boots normally (initramfs, init, service manager, shell). The
script waits for each shell prompt on the serial console, types a command,
and checks the output that appears before the next prompt. The last command
is `poweroff`, after which QEMU must exit by itself.

For the network steps the QEMU command line must attach a NIC on the user
network. The script runs an HTTP server and TCP/UDP echo servers on the
host's 127.0.0.1, which JellyOS reaches as 10.0.2.2.

With --qmp (QEMU started with -qmp unix:SOCKET,server,nowait and a VirtIO
keyboard and tablet) the graphical steps run: the script starts guidemo,
types and clicks through QMP input events, follows guidemo's output on the
console and checks screenshots.
Exit status 0 means every check passed.
"""

import http.server
import json
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

    def wait_for(self, text, timeout):
        """Read until `text` appears anywhere; keep what follows it."""
        deadline = time.monotonic() + timeout
        needle = text.encode()
        while True:
            clean = ANSI.sub(b"", self.buffer).replace(b"\r", b"")
            position = clean.find(needle)
            if position >= 0:
                self.buffer = clean[position + len(needle):]
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(clean[-400:].decode(errors="replace"))
            ready, _, _ = select.select([self.process.stdout], [], [], min(remaining, 0.5))
            if ready:
                data = os.read(self.process.stdout.fileno(), 4096)
                if not data:
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


class Qmp:
    """QEMU machine protocol: input events and screenshots."""

    # Physical keys for characters with the German layout displayd uses (keymap=de).
    KEYS = {" ": ("spc", False), "/": ("7", True), ".": ("dot", False), "-": ("slash", False),
            "\n": ("ret", False), "y": ("z", False), "z": ("y", False)}

    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.socket.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.file = self.socket.makefile("rb")
        self.file.readline()  # greeting
        self.command("qmp_capabilities")
        self.width, self.height = 1280, 800

    def command(self, name, **arguments):
        self.socket.sendall(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply or "error" in reply:
                if "error" in reply:
                    raise RuntimeError(f"QMP {name}: {reply['error']}")
                return reply["return"]

    def events(self, *events):
        self.command("input-send-event", events=list(events))
        time.sleep(0.05)

    def move(self, x, y):
        self.events({"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (self.width - 1))}},
                    {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (self.height - 1))}})

    def button(self, down, button="left"):
        self.events({"type": "btn", "data": {"down": down, "button": button}})

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        self.button(False)
        time.sleep(0.3)

    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0)
        self.button(True)
        for step in range(1, 6):
            self.move(x0 + (x1 - x0) * step // 5, y0 + (y1 - y0) * step // 5)
        self.button(False)
        time.sleep(0.3)

    def key(self, qcode, shift=False):
        def event(code, down):
            return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": code}}}
        if shift:
            self.events(event("shift", True))
        self.events(event(qcode, True))
        self.events(event(qcode, False))
        if shift:
            self.events(event("shift", False))

    def type(self, text):
        for c in text:
            if c in self.KEYS:
                self.key(*self.KEYS[c])
            elif c.isupper():
                self.key(self.KEYS.get(c.lower(), (c.lower(), False))[0], True)
            else:
                self.key(c)

    def screenshot(self):
        path = os.path.abspath("build/gui-test.ppm")
        self.command("screendump", filename=path)
        data = open(path, "rb").read()
        header = data.split(b"\n", 3)
        self.width, self.height = map(int, header[1].split())
        return Screenshot(self.width, header[3])


class Screenshot:
    def __init__(self, width, pixels):
        self.width = width
        self.pixels = pixels

    def color(self, x, y):
        i = (y * self.width + x) * 3
        r, g, b = self.pixels[i:i + 3]
        return r << 16 | g << 8 | b


# Window placement follows the compositor's cascade: the terminal (started by
# displayd) is the first window, guidemo the second.
DEMO_X, DEMO_Y = 92, 110            # guidemo's content area
TITLE_FOCUSED, TITLE_UNFOCUSED = 0x7C3AED, 0x3B3B4F
DARK_BACKGROUND = 0x1B1A26


def gui_steps(console, qmp, timeout):
    """Milestone M8: graphical applications run. Returns the number of failures."""
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        if condition:
            print(f"shell test: ok    gui: {name}")
        else:
            failures += 1
            print(f"shell test: FAIL  gui: {name} {detail}")

    def expect(name, text):
        try:
            console.wait_for(text, 30)
            check(name, True)
        except TimeoutError as error:
            check(name, False, f"(no '{text}'; console: {str(error)[-300:]!r})")

    console.send("guidemo &")
    expect("guidemo starts and connects to the display server", "guidemo: ready")
    time.sleep(1.0)
    shot = qmp.screenshot()
    check("the new window has the focus", shot.color(300, 96) == TITLE_FOCUSED,
          f"(title bar {shot.color(300, 96):06x})")
    check("the terminal lost the focus", shot.color(400, 64) == TITLE_UNFOCUSED,
          f"(title bar {shot.color(400, 64):06x})")

    qmp.type("Jelly\n")
    expect("keyboard input reaches the text field", "guidemo: greeted 'Jelly'")
    qmp.click(DEMO_X + 60, DEMO_Y + 166)
    expect("a click on a button", "guidemo: clicked 1")
    qmp.click(DEMO_X + 60, DEMO_Y + 166)
    expect("a second click", "guidemo: clicked 2")
    qmp.click(DEMO_X + 146, DEMO_Y + 167)  # the box of "Dark mode" (moved right by the wider button text)
    expect("a checkbox switches the theme", "guidemo: dark mode on")
    time.sleep(1.0)
    shot = qmp.screenshot()
    check("dark mode is drawn", shot.color(DEMO_X + 432, DEMO_Y + 300) == DARK_BACKGROUND,
          f"(background {shot.color(DEMO_X + 432, DEMO_Y + 300):06x})")
    qmp.key("tab")
    qmp.key("spc")
    expect("Tab moves the focus, Space activates", "guidemo: large text on")

    qmp.drag(300, 96, 700, 300)
    time.sleep(1.0)
    shot = qmp.screenshot()
    check("dragging the title bar moves the window", shot.color(850, 296) == TITLE_FOCUSED,
          f"(at the new place {shot.color(850, 296):06x})")
    qmp.click(915, 300)
    time.sleep(1.5)
    shot = qmp.screenshot()
    check("the close button closes the window", shot.color(850, 296) != TITLE_FOCUSED)
    check("the terminal gets the focus back", shot.color(400, 64) == TITLE_FOCUSED,
          f"(title bar {shot.color(400, 64):06x})")

    qmp.type("touch /tmp/fromgui\n")
    time.sleep(2.0)
    console.send("ls /tmp")
    output = console.read_until(PROMPT, timeout)
    check("the terminal runs commands typed on the keyboard", "fromgui" in output, f"(ls /tmp: {output!r})")
    return failures


def main():
    args = sys.argv[1:]
    timeout = 90.0
    qmp_path = None
    while args[:1] in (["--timeout"], ["--qmp"]):
        if args[0] == "--timeout":
            timeout = float(args[1])
        else:
            qmp_path = args[1]
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

    qmp = None
    try:
        console.read_until(PROMPT, timeout)
        print("shell test: shell prompt reached")
        if qmp_path:
            qmp = Qmp(qmp_path)
            steps.insert(len(steps) - 1, "gui")
        for step in steps:
            if step == "gui":
                failures += gui_steps(console, qmp, timeout)
                continue
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
