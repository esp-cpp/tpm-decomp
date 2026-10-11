#!/usr/bin/env python3
"""Compare one function of the build against the original executable.

    tools/fdiff.py File_Init [more names...]

The built address comes from build/us/tpm.elf's symbol table, the original's
from config/symbols*.txt (or a func_XXXXXXXX name). Relocated fields (j/jal
targets, %hi/%lo immediates) are compared loosely; register and opcode
differences are flagged.
"""
import re
import subprocess
import sys

import rabbitizer

TEXT, HDR = 0x80010000, 0x800
NM = "tools/binutils/bin/mips-linux-gnu-nm"


def symbols_from_config():
    out = {}
    for path in ("config/symbols.us.txt", "config/symbols.psyq.us.txt"):
        for line in open(path):
            m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)", line)
            if m:
                out[m.group(1)] = int(m.group(2), 16)
    return out


def built_symbols(elf):
    out = {}
    for line in subprocess.run([NM, "-S", elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            out[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return out


def words(data, vram, size):
    off = vram - TEXT + HDR
    return [int.from_bytes(data[off + i:off + i + 4], "little") for i in range(0, size, 4)]


def loose(a, b):
    if a == b:
        return True
    if (a >> 26) == (b >> 26) and (a >> 26) in (2, 3):
        return True
    return (a >> 16) == (b >> 16)


def main():
    orig = open("disks/us/SLUS_008.84", "rb").read()
    built = open("build/us/SLUS_008.84", "rb").read()
    cfg = symbols_from_config()
    bsyms = built_symbols("build/us/tpm.elf")
    for name in sys.argv[1:]:
        if name not in bsyms:
            print(name, ": not in the build")
            continue
        bvram, size = bsyms[name]
        ovram = cfg.get(name) or int(name[5:], 16)
        a, b = words(built, bvram, size), words(orig, ovram, size)
        bad = sum(not loose(x, y) for x, y in zip(a, b))
        print("%s: built %08X original %08X size 0x%X: %s" % (
            name, bvram, ovram, size, "MATCH" if bad == 0 else "%d differing" % bad))
        if bad:
            for i, (x, y) in enumerate(zip(a, b)):
                ix = rabbitizer.Instruction(x, vram=bvram + 4 * i)
                iy = rabbitizer.Instruction(y, vram=ovram + 4 * i)
                mark = "  " if loose(x, y) else "<>"
                print("  %s %-40s %s" % (mark, ix.disassemble(), iy.disassemble()))


if __name__ == "__main__":
    main()
