#!/usr/bin/env python3
"""Integration test for milestones M6 to M10: the shell, the network, graphical applications, sound.

usage: shell_test.py [--timeout SECONDS] [--qmp SOCKET] [--wav FILE] [--gpu] -- QEMU COMMAND LINE...

The machine boots normally (initramfs, init, service manager, shell). The
script waits for each shell prompt on the serial console, types a command,
and checks the output that appears before the next prompt. The last command
is `poweroff`, after which QEMU must exit by itself.

For the network steps the QEMU command line must attach a NIC on the user
network. The script runs an HTTP server and TCP/UDP echo servers on the
host's 127.0.0.1, which JellyOS reaches as 10.0.2.2.

With --qmp (QEMU started with -qmp unix:SOCKET,server,nowait and a USB
keyboard and tablet) the graphical steps run: the script starts guidemo,
types and clicks through QMP input events, follows guidemo's output on the
console and checks screenshots.

With an intel-hda sound card on the command line the audio steps run. With
--wav (the file QEMU's "wav" audio backend writes) the script analyzes
what JellyOS played after QEMU has exited: which tones, how loud, and that
two programs were mixed.

With --gpu (and --qmp; QEMU started with -vga none -device virtio-vga) only
the steps for the VirtIO GPU driver run: that the host shows the guest's
frames, the card's own pointer, and modes.
Exit status 0 means every check passed.
"""

import array
import http.server
import json
import math
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

# Kernel messages may follow the prompt: USB devices behind hubs are found while the shell already runs.
PROMPT = re.compile(rb"jelly:[^\r\n#]*# (?:\[ *[0-9.]+\] [^\n]*\n)*$")
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
        # (which card is eth0 depends on the order the drivers start in)
        ("ifconfig", ["inet 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3"], 30),
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


def intel_nic_steps(http_port):
    """Phase 12: the Intel e1000e driver, as a second card on its own network (the host is 10.0.3.2 there)."""
    return [
        ("dmesg e1000:", ["Intel 82574L", "link up"]),
        # The display server reports in the kernel log how it shows frames (no graphics driver in QEMU).
        ("dmesg displayd", ["plain framebuffer, mode switching, waiting for clients"]),
        ("ifconfig", ["10.0.2.15/24", "inet 10.0.3.15/24 gateway 10.0.3.2"], 30),
        ("ping -c 3 -i 0.2 10.0.3.2", ["3 packets transmitted, 3 received"]),
        (f"http http://10.0.3.2:{http_port}/hello.txt", [HOST_TEXT]),
        (f"http -o /tmp/big.bin http://10.0.3.2:{http_port}/big.bin; ls -l /tmp/big.bin", ["300000"]),
    ]


def disk_steps():
    """Phase 12: files on the NVMe disk and the second SATA disk; the host reads them back afterwards."""
    steps = [("dmesg ahci: nvme0:", ["ahci: port 1: QEMU HARDDISK, 64 MiB", "nvme0: namespace 1: 64 MiB"])]
    steps += [
        # exFAT (read-only), a volume without a partition table
        ("ls /volumes/virtio1", ["big.bin", "docs/", "Fragmented Datei", "hello.txt"]),
        ("cat /volumes/virtio1/docs/nested/deep.txt", ["deep inside exFAT"]),
        ("cp /etc/motd /volumes/virtio1/x.txt; echo code $?", ["code 1"]),
    ]
    steps += [
        # USB: a stick on a root port and one behind a hub
        ("dmesg usb: usb-storage", ["usb: hub", "usb-storage:", "64 MiB"], 10),
    ]
    for volume in ("nvme0n1p1", "ahci1p1", "usb0p1", "usb1p1"):
        steps += [
            (f"cat /volumes/{volume}/hello.txt", ["Hello from the JellyOS test disk!"]),
            (f"cp /etc/motd /volumes/{volume}/from-jelly.txt; sync; cat /volumes/{volume}/from-jelly.txt",
             ["Welcome to JellyOS"]),
        ]
    steps += [
        # Pull the stick behind the hub out while the system runs: its volume disappears, the other one stays.
        "unplug usbstick2",
        ("ls /volumes; dmesg removed", ["block usb1p1: removed", "block usb1: removed", "usb0p1"]),
        ("ls /volumes/usb1p1", ["No such file or directory"]),
        ("cat /volumes/usb0p1/hello.txt", ["Hello from the JellyOS test disk!"]),
    ]
    return steps


def audio_steps():
    """Milestone M10: the audio server, its tools and the mixer. The sound itself is checked by check_sound()."""
    return [
        ("svc status audio", ["audio", "running"], 10),
        ("volume", ["volume: 100%", "HD Audio", "48000 Hz"]),
        ("tone -f 440 -d 1000", ["tone: 440 Hz for 1000 ms"]),
        # Two programs at once; the second stream is mono at another sample rate.
        ("tone -f 1000 -d 1000 -v 70 & tone -f 2500 -d 1500 -v 70 -r 22050 -m",
         ["tone: 1000 Hz for 1000 ms", "tone: 2500 Hz for 1500 ms"]),
        ("play /usr/share/sounds/chime.wav", ["play: /usr/share/sounds/chime.wav: 22050 Hz, 1 channel, 700 ms"]),
        ("volume 50; tone -f 3500 -d 800; volume 100", ["volume: 50%", "tone: 3500 Hz for 800 ms", "volume: 100%"]),
        ("volume mute; tone -f 5000 -d 500; volume unmute",
         ["volume: 100% (muted)", "tone: 5000 Hz for 500 ms", "volume: 100%"]),
        ("record -d 1 -r 16000 -c 1 /tmp/rec.wav; ls -l /tmp/rec.wav",
         ["record: /tmp/rec.wav: 16000 frames at 16000 Hz", "32044"]),
        ("play /tmp/rec.wav", ["play: /tmp/rec.wav: 16000 Hz, 1 channel, 1000 ms"]),
        ("play /etc/motd; echo code $?", ["not a PCM WAV file", "code 1"]),
    ]


WINDOW_SECONDS = 0.05
TONES = (440, 660, 880, 1000, 2500, 3500, 5000)


def read_wav(path):
    """Left channel and sample rate of a 16-bit PCM WAV file."""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    position, rate, channels = 12, 0, 0
    while position + 8 <= len(data):
        name, size = data[position:position + 4], int.from_bytes(data[position + 4:position + 8], "little")
        position += 8
        if name == b"fmt ":
            channels = int.from_bytes(data[position + 2:position + 4], "little")
            rate = int.from_bytes(data[position + 4:position + 8], "little")
            if int.from_bytes(data[position + 14:position + 16], "little") != 16:
                raise ValueError("not 16 bits")
        elif name == b"data":
            samples = array.array("h")
            body = data[position:]  # QEMU fixes the length only when it exits cleanly: take what is there
            samples.frombytes(body[:len(body) // 2 * 2])
            if sys.byteorder == "big":
                samples.byteswap()
            return samples[::channels], rate
        position += size + (size & 1)
    raise ValueError("no sample data")


def analyze(samples, rate):
    """Per window of 50 ms: (RMS amplitude, {tone: share of the window's energy})."""
    size = int(rate * WINDOW_SECONDS)
    windows = []
    for start in range(0, len(samples) - size + 1, size):
        window = samples[start:start + size]
        energy = sum(v * v for v in window)
        shares = {}
        if energy > size * 100.0 * 100.0:  # louder than an RMS of 100: not silence
            for tone in TONES:
                # Goertzel: the energy at one frequency
                coefficient = 2 * math.cos(2 * math.pi * tone / rate)
                s1 = s2 = 0.0
                for v in window:
                    s1, s2 = v + coefficient * s1 - s2, s1
                power = s1 * s1 + s2 * s2 - coefficient * s1 * s2
                shares[tone] = 2 * power / (size * energy)
        windows.append((math.sqrt(energy / size), shares))
    return windows


def check_sound(path):
    """What JellyOS played, as recorded by QEMU. Returns the number of failures."""
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        if condition:
            print(f"shell test: ok    sound: {name}")
        else:
            failures += 1
            print(f"shell test: FAIL  sound: {name} {detail}")

    try:
        samples, rate = read_wav(path)
    except (OSError, ValueError) as error:
        check("QEMU recorded the output", False, f"({error})")
        return failures
    windows = analyze(samples, rate)
    check("QEMU recorded the output", len(samples) > rate, f"({len(samples) / rate:.1f} s)")

    def seconds(*tones, share=0.6):
        """How long these tones together made up most of the sound, each of them clearly present."""
        count = sum(1 for _, shares in windows
                    if shares and sum(shares[t] for t in tones) >= share
                    and all(shares[t] >= 0.15 for t in tones))
        return count * WINDOW_SECONDS

    def loudness(tone):
        values = sorted(rms for rms, shares in windows if shares and shares[tone] >= 0.9)
        return values[len(values) // 2] if values else 0.0

    # Durations are lower bounds: under emulation the mixer may run late, which costs a few windows.
    check("a 440 Hz tone of one second", 0.7 <= seconds(440) <= 1.3, f"({seconds(440):.2f} s)")
    full = loudness(440)
    check("the tone has the amplitude the program wrote", 9500 <= full <= 12500, f"(RMS {full:.0f}, expected 11314)")
    check("two programs are mixed", seconds(1000, 2500) >= 0.6, f"({seconds(1000, 2500):.2f} s of 1000 + 2500 Hz)")
    check("the longer stream plays on alone", seconds(2500, share=0.9) >= 0.25, f"({seconds(2500, share=0.9):.2f} s)")
    check("the WAV file is played (22050 Hz mono converted)", seconds(660) >= 0.1 and seconds(880) >= 0.2,
          f"({seconds(660):.2f} s of 660 Hz, {seconds(880):.2f} s of 880 Hz)")
    half = loudness(3500)
    check("volume 50 plays at a quarter of the amplitude", full > 0 and 0.2 <= half / full <= 0.3,
          f"(RMS {half:.0f} of {full:.0f})")
    check("a muted tone is silent", seconds(5000, share=0.2) == 0, f"({seconds(5000, share=0.2):.2f} s of 5000 Hz)")
    return failures


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


def start_host_servers():
    """HTTP server for a temporary directory plus TCP and UDP echo servers that answer in upper case."""
    directory = tempfile.mkdtemp(prefix="jellyos-http-")
    with open(os.path.join(directory, "hello.txt"), "w") as f:
        f.write(HOST_TEXT + "\n")
    with open(os.path.join(directory, "big.bin"), "wb") as f:
        f.write(bytes((i * 7 + i // 251) % 256 for i in range(300000)))

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


# Window placement follows the compositor's cascade (64-pixel steps of 32):
# the terminal (from the launcher) is the first window, guidemo the second,
# files the third, the viewer the fourth, settings the fifth.
def cascade(n):
    return 60 + 32 * n, 78 + 32 * n


DEMO_X, DEMO_Y = cascade(1)          # guidemo's content area
TITLE_FOCUSED, TITLE_UNFOCUSED = 0x7C3AED, 0x3B3B4F
DARK_BACKGROUND = 0x1B1A26
LAUNCHER = (50, 780)                 # the JellyOS button in the taskbar
MENU = {"Files": 534, "GUI demo": 562, "Gamepad": 590, "Settings": 618, "Terminal": 646, "Log out": 683}


def gui_steps(console, qmp, timeout):
    """Milestones M8 and M9: graphical applications and the desktop. Returns the number of failures."""
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        if condition:
            print(f"shell test: ok    gui: {name}")
        else:
            failures += 1
            print(f"shell test: FAIL  gui: {name} {detail}")

    def expect(name, text, wait=30):
        try:
            console.wait_for(text, wait)
            check(name, True)
            return True
        except TimeoutError as error:
            check(name, False, f"(no '{text}'; console: {str(error)[-300:]!r})")
            return False

    def launch(entry, text):
        qmp.click(*LAUNCHER)
        expect(f"the launcher opens ({entry})", "desktop: launcher open")
        time.sleep(0.5)
        qmp.click(60, MENU[entry])
        return expect(f"the launcher starts {entry}", text)

    # --- Login and session (M9)
    qmp.type("jelly\n")
    qmp.type("wrong\n")
    expect("a wrong password is refused", "login: failed for 'jelly'")
    qmp.type("jelly\n")
    expect("the right password starts the session", "login: session of jelly started (uid 1000)")
    expect("the desktop shell starts", "desktop: ready")
    time.sleep(1.5)
    shot = qmp.screenshot()
    check("the taskbar is shown", shot.color(640, 790) == DARK_BACKGROUND, f"({shot.color(640, 790):06x})")

    # --- Terminal from the launcher, running with the user's rights
    launch("Terminal", "desktop: started /bin/terminal")
    time.sleep(2.0)
    qmp.type("touch /tmp/fromgui\n")
    time.sleep(2.0)
    console.send("ls -l /tmp")
    output = console.read_until(PROMPT, timeout)
    line = next((l for l in output.splitlines() if "fromgui" in l), "")
    check("the terminal runs commands as the user", "1000" in line, f"(ls -l /tmp: {output!r})")
    # A USB stick belongs to whoever plugged it in: the user may write to its FAT volume.
    qmp.type("touch /volumes/usb0p1/fromuser\n")
    time.sleep(2.0)
    console.send("ls /volumes/usb0p1")
    output = console.read_until(PROMPT, timeout)
    check("the user can write to a USB stick", "fromuser" in output, f"(ls /volumes/usb0p1: {output!r})")

    # --- guidemo (M8): focus, keyboard, mouse, themes, moving, closing
    console.send("guidemo &")
    expect("guidemo starts and connects to the display server", "guidemo: ready")
    time.sleep(1.0)
    shot = qmp.screenshot()
    check("the new window has the focus", shot.color(300, 96) == TITLE_FOCUSED, f"(title bar {shot.color(300, 96):06x})")
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
    qmp.click(914, 300)  # close button: frame right edge (933) - 12 - 7
    time.sleep(1.5)
    shot = qmp.screenshot()
    check("the close button closes the window", shot.color(850, 296) != TITLE_FOCUSED)
    check("the terminal gets the focus back", shot.color(400, 64) == TITLE_FOCUSED,
          f"(title bar {shot.color(400, 64):06x})")

    # --- File manager and viewer
    if launch("Files", "files: showing /home/jelly (1 items)"):
        x, y = cascade(2)
        time.sleep(1.0)
        qmp.move(x + 120, y + 124)  # the first row: Welcome.txt
        for _ in range(2):
            qmp.button(True)
            qmp.button(False)
        expect("a double-click opens the text viewer", "viewer: /home/jelly/Welcome.txt")

    # --- Settings: the theme changes in every program of the session
    if launch("Settings", "settings: ready"):
        x, y = cascade(4)
        time.sleep(1.0)
        qmp.click(x + 198, y + 71)  # "Dark mode"
        expect("settings switch to the dark theme", "settings: theme dark")
        time.sleep(2.0)
        fx, fy = cascade(2)
        shot = qmp.screenshot()
        check("other programs follow the new theme", shot.color(fx + 5, fy + 200) == DARK_BACKGROUND,
              f"(files background {shot.color(fx + 5, fy + 200):06x})")
        frame_right = x + 560 + 1
        qmp.click(frame_right - 12 - 14 - 8 - 7, y - 14)  # maximize button
        time.sleep(1.5)
        shot = qmp.screenshot()
        check("maximize fills the screen above the taskbar", shot.color(640, 6) == TITLE_FOCUSED,
              f"(top of the screen {shot.color(640, 6):06x})")

    # --- The gamepad viewer starts (QEMU has no gamepad to press buttons on)
    launch("Gamepad", "gamepad: ready")
    time.sleep(0.5)

    # --- Notifications from any program
    console.send("notify Test Nachricht")
    expect("notify shows a notification", "desktop: notification 'Test'")

    # --- Screen modes while the desktop runs (Phase 12). QEMU's VGA card is driven by bochs-gpu; the Settings
    #     window is maximized and focused at this point.
    def screen_is(name, width, height):
        if not expect(f"{name}: the display server follows", f"the screen is now {width}x{height}"):
            return
        time.sleep(2.5)
        shot = qmp.screenshot()
        check(f"{name}: the screen has {width}x{height} pixels", (qmp.width, qmp.height) == (width, height),
              f"({qmp.width}x{qmp.height})")
        check(f"{name}: the maximized window fills the new screen", shot.color(width // 2, 6) == TITLE_FOCUSED,
              f"(top of the screen {shot.color(width // 2, 6):06x})")
        check(f"{name}: the taskbar is at the new bottom edge",
              shot.color(4, height - 3) == taskbar_corner and shot.color(width - 4, height - 3) == taskbar_corner,
              f"({shot.color(4, height - 3):06x} {shot.color(width - 4, height - 3):06x}, was {taskbar_corner:06x})")

    taskbar_corner = qmp.screenshot().color(4, 797)
    console.send("display")
    output = console.read_until(PROMPT, timeout)
    check("display lists the driver's modes",
          all(text in output for text in ("display 0: 1280x800", "mode switching", "1920x1080", "1024x768", "(current)")),
          f"({output!r})")
    qmp.click(55, 54)   # the first section of Settings: the list of sections has the keyboard
    qmp.key("end")      # the last section: Display
    time.sleep(1.0)
    qmp.key("tab")      # the list of modes
    qmp.key("down")     # the second mode
    expect("Settings asks for the chosen mode", "settings: display mode 1920x1080")
    screen_is("mode chosen in Settings", 1920, 1080)
    expect("Settings shows the new mode", "settings: display updated", 10)
    console.send("cat /etc/display.conf")
    output = console.read_until(PROMPT, timeout)
    check("the display server keeps the choice for the next start", "mode=1920x1080@60" in output, f"({output!r})")
    console.send("display 1024x768")
    screen_is("mode set with the display command", 1024, 768)
    console.send("display 640x400")
    expect("a mode the driver does not have is refused", "display: no mode '640x400'")
    console.send("display 0")
    screen_is("back to the first mode", 1280, 800)
    console.send("")
    console.read_until(PROMPT, timeout)

    # --- Log out: back to the login
    qmp.click(*LAUNCHER)
    expect("the launcher opens (log out)", "desktop: launcher open")
    time.sleep(0.5)
    qmp.click(60, MENU["Log out"])
    expect("logging out ends the session", "login: session of jelly ended")
    expect("the login appears again", "login: ready")
    console.send("")
    console.read_until(PROMPT, timeout)
    return failures


def gpu_steps(console, qmp, timeout):
    """Phase 12: the VirtIO GPU driver. Returns the number of failures."""
    failures = 0

    def check(name, condition, detail=""):
        nonlocal failures
        if condition:
            print(f"shell test: ok    gpu: {name}")
        else:
            failures += 1
            print(f"shell test: FAIL  gpu: {name} {detail}")

    def expect(name, text, wait=30):
        try:
            console.wait_for(text, wait)
            check(name, True)
            return True
        except TimeoutError as error:
            check(name, False, f"(no '{text}'; console: {str(error)[-300:]!r})")
            return False

    def driver_log(name):
        console.send("dmesg virtio-gpu")
        output = console.read_until(PROMPT, timeout)
        check(name, "failed" not in output and "no answer" not in output, f"({output!r})")
        return output

    # --- The driver has the screen
    output = driver_log("no command of the driver failed at the start")
    check("the driver takes the card", "VirtIO GPU 1af4:1050" in output and "2 framebuffers" in output, f"({output!r})")
    console.send("display")
    output = console.read_until(PROMPT, timeout)
    check("the display has a pointer, frame timing, flipping and modes",
          all(text in output for text in ("display 0: 1280x800 at 60.00 Hz", "hardware pointer", "vertical blank timing",
                                          "page flipping", "mode switching", "1920x1080", "(current)  (preferred)")),
          f"({output!r})")

    # --- Frames reach the host: what a screenshot of the host's side shows
    shot = qmp.screenshot()
    check("the host shows a 1280x800 picture", (qmp.width, qmp.height) == (1280, 800), f"({qmp.width}x{qmp.height})")
    colors = {shot.color(x, y) for x in range(8, qmp.width, 16) for y in range(8, qmp.height, 16)}
    check("the login screen is on it", len(colors) > 1, f"(one colour: {colors})")
    qmp.type("jelly\n")
    qmp.type("jelly\n")
    expect("logging in", "login: session of jelly started (uid 1000)")
    expect("the desktop shell starts", "desktop: ready")
    time.sleep(1.5)
    shot = qmp.screenshot()
    check("the taskbar is shown", shot.color(640, 790) == DARK_BACKGROUND, f"({shot.color(640, 790):06x})")
    taskbar_corner = shot.color(4, 797)

    # --- The pointer is the card's: the host places it, the frame does not contain it
    qmp.move(700, 300)
    time.sleep(0.7)
    under = qmp.screenshot()
    qmp.move(200, 600)
    time.sleep(0.7)
    away = qmp.screenshot()
    check("the pointer is not drawn into the frame",
          all(under.color(700 + i, 300 + j) == away.color(700 + i, 300 + j) for i in range(14) for j in range(14)))
    qmp.click(*LAUNCHER)
    expect("a click arrives where the pointer is", "desktop: launcher open")
    time.sleep(0.5)
    qmp.click(60, MENU["Terminal"])
    expect("the launcher starts the terminal", "desktop: started /bin/terminal")
    time.sleep(2.0)
    shot = qmp.screenshot()
    check("the terminal's window is on the screen", shot.color(400, 64) == TITLE_FOCUSED,
          f"(title bar {shot.color(400, 64):06x})")

    # --- Modes: a resource of another size on the host, the same framebuffers
    def screen_is(name, width, height):
        if not expect(f"{name}: the display server follows", f"the screen is now {width}x{height}"):
            return
        time.sleep(2.5)
        shot = qmp.screenshot()
        check(f"{name}: the host's picture has {width}x{height} pixels", (qmp.width, qmp.height) == (width, height),
              f"({qmp.width}x{qmp.height})")
        check(f"{name}: the taskbar is at the new bottom edge",
              shot.color(4, height - 3) == taskbar_corner and shot.color(width - 4, height - 3) == taskbar_corner,
              f"({shot.color(4, height - 3):06x} {shot.color(width - 4, height - 3):06x}, was {taskbar_corner:06x})")

    console.send("display 1024x768")
    screen_is("a smaller mode", 1024, 768)
    console.send("display 1920x1080")
    screen_is("a mode larger than the firmware's", 1920, 1080)
    console.send("display 0")
    screen_is("back to the host's mode", 1280, 800)
    console.send("")
    console.read_until(PROMPT, timeout)
    driver_log("no command of the driver failed")
    return failures


def main():
    args = sys.argv[1:]
    timeout = 90.0
    qmp_path = wav_path = None
    gpu = False
    while args[:1] in (["--timeout"], ["--qmp"], ["--wav"], ["--gpu"]):
        if args[0] == "--gpu":
            gpu = True
            args = args[1:]
            continue
        if args[0] == "--timeout":
            timeout = float(args[1])
        elif args[0] == "--wav":
            wav_path = args[1]
        else:
            qmp_path = args[1]
        args = args[2:]
    if args[:1] != ["--"] or len(args) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    os.makedirs("build", exist_ok=True)
    log_path = "build/gpu-test.log" if gpu else "build/shell-test.log"
    log = open(log_path, "wb")
    steps = [] if gpu else STEPS[:]
    if gpu:
        pass
    elif "virtio-net" in " ".join(args):
        ports = start_host_servers()
        steps += network_steps(*ports)
        if "e1000e" in " ".join(args):
            steps += intel_nic_steps(ports[0])
    if not gpu and "intel-hda" in " ".join(args):
        steps += audio_steps()
    if not gpu and "nvme" in " ".join(args):
        steps += disk_steps()
    steps.append(FINAL_STEP)
    console = Console(args[1:], log)
    failures = 0

    qmp = None
    try:
        console.read_until(PROMPT, timeout)
        print("shell test: shell prompt reached")
        if qmp_path:
            qmp = Qmp(qmp_path)
            steps.insert(len(steps) - 1, "gpu" if gpu else "gui")
        for step in steps:
            if step == "gui":
                failures += gui_steps(console, qmp, timeout)
                continue
            if step == "gpu":
                failures += gpu_steps(console, qmp, timeout)
                continue
            if isinstance(step, str) and step.startswith("unplug "):
                if qmp:
                    qmp.command("device_del", id=step.split()[1])
                    time.sleep(3.0)
                    print(f"shell test: ok    {step} (QMP)")
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
    if wav_path:
        failures += check_sound(wav_path)
    log.close()
    print(f"shell test: {'PASSED' if failures == 0 else f'{failures} FAILED'} (log: {log_path})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
