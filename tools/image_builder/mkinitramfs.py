#!/usr/bin/env python3
"""Pack a directory tree into a JellyOS initramfs (cpio "newc" format).

usage: mkinitramfs.py ROOT OUTPUT [OWNERS]

Entries keep their permission bits and belong to root (uid 0, gid 0),
unless the OWNERS file ("PATH UID GID" per line) assigns PATH and
everything below it to someone else.
Directories come before their contents; the archive ends with TRAILER!!!.
The kernel unpacks it into the root ramfs (fs/initramfs/initramfs.c).
"""

import os
import stat
import sys

MAGIC = b"070701"


def pad4(data: bytearray) -> None:
    while len(data) % 4:
        data.append(0)


OWNERS = []


def owner_of(name: str):
    for path, uid, gid in OWNERS:
        if name == path or name.startswith(path + "/"):
            return uid, gid
    return 0, 0


def entry(out: bytearray, name: str, mode: int, body: bytes, inode: int) -> None:
    encoded = name.encode() + b"\0"
    uid, gid = owner_of(name)
    fields = [
        inode,          # c_ino
        mode,           # c_mode
        uid,            # c_uid
        gid,            # c_gid
        1,              # c_nlink
        0,              # c_mtime
        len(body),      # c_filesize
        0, 0,           # c_devmajor, c_devminor
        0, 0,           # c_rdevmajor, c_rdevminor
        len(encoded),   # c_namesize
        0,              # c_check
    ]
    out += MAGIC + b"".join(b"%08X" % f for f in fields)
    out += encoded
    pad4(out)
    out += body
    pad4(out)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    root, output = sys.argv[1], sys.argv[2]
    if len(sys.argv) == 4:
        for line in open(sys.argv[3]):
            words = line.split("#")[0].split()
            if len(words) == 3:
                OWNERS.append((words[0].strip("/"), int(words[1]), int(words[2])))
    out = bytearray()
    inode = 1

    for directory, subdirs, files in os.walk(root):
        subdirs.sort()
        relative = os.path.relpath(directory, root)
        if relative != ".":
            info = os.lstat(directory)
            entry(out, relative, stat.S_IFDIR | stat.S_IMODE(info.st_mode), b"", inode)
            inode += 1
        for name in sorted(files):
            path = os.path.join(directory, name)
            archived = os.path.normpath(os.path.join(relative, name))
            info = os.lstat(path)
            if stat.S_ISLNK(info.st_mode):
                body = os.readlink(path).encode()
                mode = stat.S_IFLNK | 0o777
            else:
                with open(path, "rb") as f:
                    body = f.read()
                mode = stat.S_IFREG | stat.S_IMODE(info.st_mode)
            entry(out, archived, mode, body, inode)
            inode += 1

    entry(out, "TRAILER!!!", 0, b"", 0)
    with open(output, "wb") as f:
        f.write(out)
    print(f"mkinitramfs: {inode - 1} entries, {len(out) // 1024} KiB -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
