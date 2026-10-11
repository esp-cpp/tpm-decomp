#!/usr/bin/env python3
"""The game's own file system: the name table in SLUS_008.84 into FILE.000.

The executable addresses data by PC-style paths (c:\\zanzibar\\data\\levels\\
%s.LVL, ...). On the disc those resolve through a table of 107 records

    struct FileEntry { u8 msf[3]; u8 pad; u32 size; char name[16]; };

at 0x800715A0 (file offset 0x61DA0 in USA Rev 1). msf is a BCD
minute:second:frame position *inside FILE.000*, with the 2-second lead-in
(00:02:00 is FILE.000's first sector). Names are unique without their
directory, so the table holds names only.

    tools/tpm_files.py list    <SLUS_008.84>
    tools/tpm_files.py extract <SLUS_008.84> <FILE.000> <outdir>
"""
import os
import struct
import sys

TABLE_VRAM = 0x800715A0
EXE_TEXT = 0x80010000
HEADER = 0x800


def bcd(b):
    return (b >> 4) * 10 + (b & 15)


def entries(exe):
    off = TABLE_VRAM - EXE_TEXT + HEADER
    while True:
        m, s, f, _ = exe[off:off + 4]
        size = struct.unpack_from("<I", exe, off + 4)[0]
        name = exe[off + 8:off + 24].split(b"\0")[0]
        if not name or not all(32 < c < 127 for c in name):
            return
        sector = (bcd(m) * 60 + bcd(s)) * 75 + bcd(f) - 150
        yield name.decode(), sector, size
        off += 24


def main():
    cmd = sys.argv[1]
    exe = open(sys.argv[2], "rb").read()
    if cmd == "list":
        for name, sector, size in entries(exe):
            print("%-16s sector %6d size %9d" % (name, sector, size))
    elif cmd == "extract":
        src = open(sys.argv[3], "rb")
        out = sys.argv[4]
        os.makedirs(out, exist_ok=True)
        for name, sector, size in entries(exe):
            src.seek(sector * 2048)
            with open(os.path.join(out, name), "wb") as fh:
                fh.write(src.read(size))
        print("extracted to", out)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
