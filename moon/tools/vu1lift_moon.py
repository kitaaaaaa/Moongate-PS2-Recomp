#!/usr/bin/env python3
"""Lifts Virtual-On's VU1 microprogram from the ELF (setup.sh runs this).

Usage: vu1lift_moon.py <ELF> <config/vu1_targets.json> <work dir> [--out <file>]
The game has one VU1 program, uploaded by consecutive VIF MPG packets. vu1_targets.json says where they sit in the
ELF and what a census run (PS2X_VU1_CENSUS) saw execute: entries, executed pairs, jumps (addresses only). This
rebuilds that census folder from the ELF and runs vu1lift_marz.py on it, giving the same file as lifting the census.
"""
import json
import os
import struct
import subprocess
import sys
import zlib


def main():
    args = sys.argv[1:]
    out = []
    if "--out" in args:
        i = args.index("--out")
        out = ["--out", args[i + 1]]
        del args[i:i + 2]
    elf_path, cfg_path, work = args
    cfg = json.load(open(cfg_path))
    elf = open(elf_path, "rb").read()
    image = bytearray(16384)
    p = int(cfg["elf_offset_first_mpg"], 16)
    for _ in range(cfg["mpg_packets"]):
        cmd = struct.unpack_from("<I", elf, p)[0]
        if (cmd >> 24) & 0x7F != 0x4A:
            sys.exit("no VIF MPG at ELF offset %#x: not the SLPM-62767 ELF" % p)
        num, addr = ((cmd >> 16) & 0xFF) or 256, (cmd & 0xFFFF) * 8
        image[addr:addr + num * 8] = elf[p + 4:p + 4 + num * 8]
        p += 4 + num * 8
        while p < len(elf) and struct.unpack_from("<I", elf, p)[0] == 0:  # VIF NOPs between packets
            p += 4
    if "%08x" % zlib.crc32(image[:cfg["code_hi"]]) != cfg["crc32"]:
        sys.exit("VU1 program checksum mismatch: not the SLPM-62767 ELF")

    os.makedirs(work, exist_ok=True)
    crc = cfg["census_crc"]
    name = "vu1_%s.bin" % crc
    with open(os.path.join(work, name), "wb") as f:
        f.write(image)
    with open(os.path.join(work, "manifest.json"), "w") as f:
        json.dump({"programs": [{"crc": crc, "image": name, "code_hi": cfg["code_hi"], "instructions": 0,
                                 "entries": [{"pc": int(e, 16)} for e in cfg["entries"]]}]}, f)
    with open(os.path.join(work, "gitaroo_census.json"), "w") as f:
        json.dump({"images": [{"crc": crc, "pc_count": {str(int(e, 16)): 1 for e in cfg["executed"]}}],
                   "jumps": [[int(a, 16), int(t, 16), n] for a, t, n in cfg["jumps"]]}, f)
    lifter = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vu1lift_marz.py")
    subprocess.run([sys.executable, lifter] + out + [work], check=True)


if __name__ == "__main__":
    main()
