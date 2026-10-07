#!/usr/bin/env python3
"""Build a small exFAT volume for the file system tests.

    mkexfat.py OUTPUT.img

The image is the bare volume (no partition table), 16 MiB, 512-byte sectors,
2048-byte clusters. It is written from the exFAT specification independently
of the kernel driver and contains what the driver has to cope with:

  hello.txt                 small, contiguous ("NoFatChain")
  big.bin                   300000 bytes of the test pattern, contiguous
  Fragmented Datei äöü €.bin  10000 bytes of the pattern in scattered clusters (FAT chain)
  \U0001F600.txt                  a name outside the Basic Multilingual Plane (UTF-16 surrogates)
  sparse.bin                5000 bytes, of which only 1000 are valid data (the rest reads as zeros)
  docs/nested/deep.txt      directories
  file00.txt ... file39.txt enough entries for a root directory of several, scattered clusters
  (a deleted file's entries in between)
"""

import struct
import sys

SECTOR = 512
CLUSTER_SECTORS = 4
CLUSTER = SECTOR * CLUSTER_SECTORS
VOLUME_SECTORS = 32768
FAT_OFFSET = 128
FAT_SECTORS = 64
HEAP_OFFSET = 256
CLUSTER_COUNT = (VOLUME_SECTORS - HEAP_OFFSET) // CLUSTER_SECTORS

END = 0xFFFFFFFF


def pattern(size):
    return bytes((i * 7 + i // 251) % 256 for i in range(size))


class Volume:
    def __init__(self):
        self.data = bytearray(VOLUME_SECTORS * SECTOR)
        self.fat = [0] * (CLUSTER_COUNT + 2)
        self.fat[0] = 0xFFFFFFF8
        self.fat[1] = END
        self.used = set()
        self.next_free = 2

    def allocate(self, count):
        """`count` clusters in a row."""
        while any(c in self.used for c in range(self.next_free, self.next_free + count)):
            self.next_free += 1
        clusters = list(range(self.next_free, self.next_free + count))
        self.used.update(clusters)
        self.next_free += count
        return clusters

    def claim(self, clusters):
        assert not self.used.intersection(clusters)
        self.used.update(clusters)
        return list(clusters)

    def write_clusters(self, clusters, content, chain):
        for i, cluster in enumerate(clusters):
            offset = (HEAP_OFFSET + (cluster - 2) * CLUSTER_SECTORS) * SECTOR
            self.data[offset:offset + CLUSTER] = content[i * CLUSTER:(i + 1) * CLUSTER].ljust(CLUSTER, b'\0')
            if chain:
                self.fat[cluster] = clusters[i + 1] if i + 1 < len(clusters) else END

    def finish(self, root_cluster):
        # Allocation bitmap (cluster 2) and FAT
        bitmap = bytearray((CLUSTER_COUNT + 7) // 8)
        for cluster in self.used:
            bitmap[(cluster - 2) // 8] |= 1 << ((cluster - 2) % 8)
        self.write_clusters([2], bytes(bitmap), True)
        fat = b''.join(struct.pack('<I', entry) for entry in self.fat)
        self.data[FAT_OFFSET * SECTOR:FAT_OFFSET * SECTOR + len(fat)] = fat

        boot = bytearray(SECTOR)
        boot[0:3] = b'\xEB\x76\x90'
        boot[3:11] = b'EXFAT   '
        struct.pack_into('<QQIIIIIIHHBBBBB', boot, 64, 0, VOLUME_SECTORS, FAT_OFFSET, FAT_SECTORS, HEAP_OFFSET,
                         CLUSTER_COUNT, root_cluster, 0x4A454C4C, 0x0100, 0, 9, 2, 1, 0x80,
                         100 * len(self.used) // CLUSTER_COUNT)
        boot[510:512] = b'\x55\xAA'
        region = bytearray(12 * SECTOR)
        region[0:SECTOR] = boot
        for extended in range(1, 9):
            region[extended * SECTOR + 510:extended * SECTOR + 512] = b'\x55\xAA'
        checksum = 0
        for i in range(11 * SECTOR):
            if i in (106, 107, 112):  # volume flags and percent in use are not covered
                continue
            checksum = ((checksum << 31) | (checksum >> 1)) + region[i] & 0xFFFFFFFF
        region[11 * SECTOR:12 * SECTOR] = struct.pack('<I', checksum) * (SECTOR // 4)
        self.data[0:12 * SECTOR] = region          # main boot region
        self.data[12 * SECTOR:24 * SECTOR] = region  # backup


def upcase(char):
    return char.upper() if len(char.upper()) == 1 else char


def name_hash(name):
    value = 0
    for unit in struct.unpack('<%dH' % (len(name.encode('utf-16-le')) // 2), upcase_name(name).encode('utf-16-le')):
        for byte in (unit & 0xFF, unit >> 8):
            value = (((value << 15) | (value >> 1)) + byte) & 0xFFFF
    return value


def upcase_name(name):
    return ''.join(upcase(c) for c in name)


def entry_set(name, attributes, first_cluster, size, valid_size, contiguous):
    units = name.encode('utf-16-le')
    count = len(units) // 2
    names = [units[i:i + 30].ljust(30, b'\0') for i in range(0, len(units), 30)]
    file_entry = bytearray(32)
    file_entry[0] = 0x85
    file_entry[1] = 1 + len(names)
    struct.pack_into('<H', file_entry, 4, attributes)
    struct.pack_into('<III', file_entry, 8, 0x5B470000, 0x5B470000, 0x5B470000)  # timestamps: 2025-10-07
    stream = bytearray(32)
    stream[0] = 0xC0
    stream[1] = 0x01 | (0x02 if contiguous else 0)  # allocation possible, NoFatChain
    stream[3] = count
    struct.pack_into('<H', stream, 4, name_hash(name))
    struct.pack_into('<Q', stream, 8, valid_size)
    struct.pack_into('<IQ', stream, 20, first_cluster, size)
    entries = file_entry + stream + b''.join(b'\xC1\x00' + part for part in names)
    checksum = 0
    for i, byte in enumerate(entries):
        if i in (2, 3):
            continue
        checksum = (((checksum << 15) | (checksum >> 1)) + byte) & 0xFFFF
    struct.pack_into('<H', entries, 2, checksum)
    return bytes(entries)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    v = Volume()
    v.claim([2])                      # allocation bitmap
    upcase_cluster = v.claim([3])     # up-case table: identity except a-z
    table = b''.join(struct.pack('<H', ord(chr(c).upper()) if 97 <= c <= 122 else c) for c in range(128))
    v.write_clusters(upcase_cluster, table, True)
    table_checksum = 0
    for byte in table:
        table_checksum = (((table_checksum << 31) | (table_checksum >> 1)) + byte) & 0xFFFFFFFF

    def add_file(content, valid=None, scattered=None):
        size = len(content)
        count = max(1, (size + CLUSTER - 1) // CLUSTER)
        clusters = v.claim(scattered) if scattered else v.allocate(count)
        assert len(clusters) == count
        v.write_clusters(clusters, content, chain=bool(scattered))
        return clusters[0], size, size if valid is None else valid, not scattered

    def directory(entries):
        content = b''.join(entries)
        clusters = v.allocate(max(1, (len(content) + CLUSTER - 1) // CLUSTER))
        v.write_clusters(clusters, content, chain=False)
        return clusters[0], len(clusters) * CLUSTER

    def file_entries(name, *described, attributes=0x20):
        first, size, valid, contiguous = described
        return entry_set(name, attributes, first, size, valid, contiguous)

    # Clusters 40-99 are set aside for scattered chains.
    v.next_free = 100
    deep = file_entries('deep.txt', *add_file(b'deep inside exFAT\n'))
    nested_cluster, nested_size = directory([deep])
    nested = entry_set('nested', 0x10, nested_cluster, nested_size, nested_size, True)
    docs_cluster, docs_size = directory([nested])

    root = [
        b'\x83\x09' + 'JELLYEXFT'.encode('utf-16-le').ljust(30, b'\0'),                       # volume label
        struct.pack('<BB18xIQ', 0x81, 0, 2, (CLUSTER_COUNT + 7) // 8),                        # allocation bitmap
        struct.pack('<B3xI12xIQ', 0x82, table_checksum, 3, len(table)),                       # up-case table
        file_entries('hello.txt', *add_file(b'Hello from an exFAT volume!\n')),
        entry_set('docs', 0x10, docs_cluster, docs_size, docs_size, True),
        file_entries('big.bin', *add_file(pattern(300000))),
        file_entries('Fragmented Datei äöü €.bin',
                     *add_file(pattern(10000), scattered=[50, 47, 48, 60, 41])),
        file_entries('\U0001F600.txt', *add_file(b'smile\n')),
    ]
    # A deleted file: the same entries with the "in use" bit cleared
    deleted = bytearray(file_entries('deleted.txt', *add_file(b'gone\n')))
    for i in range(0, len(deleted), 32):
        deleted[i] &= 0x7F
    root.append(bytes(deleted))
    sparse = bytearray(5000)
    sparse[:1000] = pattern(1000)
    sparse[1000:] = b'\xAA' * 4000  # on disk, but beyond the valid data length: must read as zeros
    root.append(file_entries('sparse.bin', *add_file(bytes(sparse), valid=1000)))
    for i in range(40):
        root.append(file_entries('file%02d.txt' % i, *add_file(b'file number %d\n' % i)))

    content = b''.join(root)
    root_clusters = v.claim([70, 45, 72, 44][:(len(content) + CLUSTER - 1) // CLUSTER])
    assert len(root_clusters) * CLUSTER >= len(content) and len(root_clusters) >= 2
    v.write_clusters(root_clusters, content, chain=True)
    v.finish(root_clusters[0])
    with open(sys.argv[1], 'wb') as out:
        out.write(v.data)


if __name__ == '__main__':
    main()
