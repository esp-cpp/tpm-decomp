"""List or extract the files of a PlayStation disc image (raw 2352-byte .bin).

    python extract_disc.py <disc.bin>                 # list
    python extract_disc.py <disc.bin> <outdir>        # extract everything

Form 1 files are written as their 2048-byte user data. Files whose sectors are
Mode 2 Form 2 (XA audio, STR movies) are written as raw 2336-byte sectors
(subheader + data), which is what the game's CD streaming code reads.
"""
import os
import struct
import sys

SECTOR = 2352


class Disc(object):
    def __init__(self, path):
        self.fh = open(path, "rb")

    def raw(self, n):
        self.fh.seek(n * SECTOR)
        return self.fh.read(SECTOR)

    def sector(self, n):
        return self.raw(n)[24:24 + 2048]

    def is_form2(self, n):
        sub = self.raw(n)[16:24]
        return bool(sub[2] & 0x20)

    def listdir(self, lba, length):
        buf = b"".join(self.sector(lba + i) for i in range((length + 2047) // 2048))
        out, i = [], 0
        while i < len(buf):
            rec_len = buf[i]
            if rec_len == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = buf[i:i + rec_len]
            ext = struct.unpack_from("<I", rec, 2)[0]
            size = struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            i += rec_len
            if name in (b"\x00", b"\x01"):
                continue
            out.append((name.decode("ascii").split(";")[0], ext, size, bool(flags & 2)))
        return out

    def walk(self, lba=None, length=None, prefix=""):
        if lba is None:
            pvd = self.sector(16)
            assert pvd[1:6] == b"CD001", "not an ISO9660 image"
            root = pvd[156:156 + 34]
            lba = struct.unpack_from("<I", root, 2)[0]
            length = struct.unpack_from("<I", root, 10)[0]
        for name, ext, size, is_dir in self.listdir(lba, length):
            path = prefix + name
            if is_dir:
                yield from self.walk(ext, size, path + "/")
            else:
                yield path, ext, size


def main():
    disc = Disc(sys.argv[1])
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    for path, lba, size in disc.walk():
        form2 = size and disc.is_form2(lba)
        print("%-40s lba %7d size %10d%s" % (path, lba, size, "  (form2)" if form2 else ""))
        if not outdir:
            continue
        dst = os.path.join(outdir, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        nsec = (size + 2047) // 2048
        with open(dst, "wb") as out:
            if form2:
                for s in range(nsec):
                    out.write(disc.raw(lba + s)[16:16 + 2336])
            else:
                left = size
                for s in range(nsec):
                    out.write(disc.sector(lba + s)[:min(2048, left)])
                    left -= 2048


if __name__ == "__main__":
    main()
