#!/usr/bin/env python3
"""Copies every file on the Virtual-On (Sega Ages 2500 Vol. 31) disc image into a directory.

Usage: extract_iso.py <disc.bin|disc.iso> <output dir>
Reads raw 2352-byte MODE2 sectors (user data at byte 24, 2048 bytes) or plain 2048-byte images.
The image is only ever read. Existing files of the right size are left alone.
"""
import os
import struct
import sys

SECTOR = 2048


class Disc:
    def __init__(self, f):
        self.f = f
        f.seek(0, 2)
        size = f.tell()
        # Raw images start each sector with the 12-byte sync pattern.
        f.seek(0)
        self.raw = f.read(12) == b'\x00' + b'\xff' * 10 + b'\x00' and size % 2352 == 0
        self.stride, self.skip = (2352, 24) if self.raw else (2048, 0)

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            self.f.seek(lba * self.stride + self.skip)
            out += self.f.read(SECTOR)
            lba += 1
        return bytes(out[:size])


def walk(disc, lba, size, path, out):
    data = disc.read(lba, size)
    off = 0
    while off < len(data):
        length = data[off]
        if length == 0:
            off = (off // SECTOR + 1) * SECTOR
            continue
        rec = data[off:off + length]
        off += length
        name = rec[33:33 + rec[32]]
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii').split(';')[0]
        e_lba = struct.unpack('<I', rec[2:6])[0]
        e_size = struct.unpack('<I', rec[10:14])[0]
        full = f'{path}/{name}'
        if rec[25] & 2:
            walk(disc, e_lba, e_size, full, out)
        else:
            out.append((full.lstrip('/'), e_lba, e_size))


def main():
    img, out_dir = sys.argv[1:3]
    with open(img, 'rb') as f:
        disc = Disc(f)
        pvd = disc.read(16, SECTOR)
        assert pvd[1:6] == b'CD001', 'no ISO9660 volume descriptor'
        root = pvd[156:190]
        entries = []
        walk(disc, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '', entries)
        for rel, lba, size in entries:
            dest = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest) and os.path.getsize(dest) == size:
                continue
            with open(dest, 'wb') as o:
                left = size
                while left:
                    n = min(left, 4096 * SECTOR)
                    o.write(disc.read(lba, n))
                    lba += n // SECTOR
                    left -= n
            print(rel, size)
    print('raw sectors:', disc.raw, 'files:', len(entries))


if __name__ == '__main__':
    main()
