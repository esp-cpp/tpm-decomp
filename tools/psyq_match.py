#!/usr/bin/env python3
"""Find the PSY-Q library objects linked into a PS-X EXE.

Uses lab313ru's psx_psyq_signatures (JSON, one file per .LIB/.OBJ per SDK
version; https://github.com/lab313ru/psx_psyq_signatures -- clone it yourself,
it is not vendored here). Every object signature is a masked byte pattern; a
hit pins the object's address and therefore every public label inside it.

    tools/psyq_match.py <exe> <sigs-dir> [--version 440] [--symbols out.txt]
                         [--segments out.yaml]

Without --version every SDK version is tried and ranked by matched bytes; the
best one is used for the outputs. --symbols writes splat symbol_addrs lines
(functions only: the loc_/text_ labels are branch targets). --segments writes
the matched objects as splat subsegment lines, in address order, for pasting
into the config.
"""
import argparse
import json
import os
import re
import struct
import sys

HEADER = 0x800


def load_exe(path):
    data = open(path, "rb").read()
    if data[:8] != b"PS-X EXE":
        sys.exit("not a PS-X EXE")
    t_addr, t_size = struct.unpack_from("<II", data, 0x18)
    return data[HEADER:HEADER + t_size], t_addr


def sig_regex(sig):
    out = []
    for tok in sig.split():
        out.append(b"." if tok == "??" else re.escape(bytes([int(tok, 16)])))
    return re.compile(b"".join(out), re.DOTALL)


def fixed_bytes(sig):
    return sum(1 for t in sig.split() if t != "??")


def scan_version(text, vdir):
    hits = []  # (offset, lib, obj, sig_len, labels)
    for lib in sorted(os.listdir(vdir)):
        if not lib.endswith(".json"):
            continue
        try:
            objs = json.load(open(os.path.join(vdir, lib)))
        except ValueError:
            continue
        libname = lib[:-len(".json")]
        for obj in objs:
            sig = obj.get("sig", "")
            # Short signatures (stubs, BIOS call thunks) match all over the place;
            # require some substance, and a unique, word-aligned position.
            if fixed_bytes(sig) < 12:
                continue
            found = [m.start() for m in sig_regex(sig).finditer(text) if m.start() % 4 == 0]
            if len(found) != 1:
                continue
            hits.append((found[0], libname, obj["name"], len(sig.split()), obj.get("labels", [])))
    return hits


def is_function_label(name):
    return not re.match(r"^(loc|text|data|rdata|sdata|bss|sbss|jpt|jtbl)_", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("sigs")
    ap.add_argument("--version")
    ap.add_argument("--symbols")
    ap.add_argument("--segments")
    ap.add_argument("--union", action="store_true",
                    help="games often link libraries from several SDK releases: "
                         "take every unique hit from every version (newest first)")
    args = ap.parse_args()

    text, vram = load_exe(args.exe)
    versions = [args.version] if args.version else sorted(
        d for d in os.listdir(args.sigs) if d.isdigit())
    scores = []
    for v in versions:
        hits = scan_version(text, os.path.join(args.sigs, v))
        score = sum(h[3] for h in hits)
        scores.append((score, v, hits))
        print("version %-5s objects %4d  bytes %7d" % (v, len(hits), score), file=sys.stderr)
    scores.sort(reverse=True)
    score, version, hits = scores[0]
    print("best: %s" % version, file=sys.stderr)
    if args.union:
        # longest signature wins at an address; the best version breaks ties
        pool = {}
        for _, v, vh in [scores[0]] + sorted(scores[1:], key=lambda s: s[1], reverse=True):
            for h in vh:
                if h[0] not in pool or h[3] > pool[h[0]][3]:
                    pool[h[0]] = h + (v,)
        hits = list(pool.values())
        version = "mixed (best %s)" % version
    else:
        hits = [h + (version,) for h in hits]

    # Overlapping hits (the same object in two libs, e.g. LIBC/LIBC2): keep the longer.
    hits.sort(key=lambda h: (h[0], -h[3]))
    kept, end = [], -1
    for h in hits:
        if h[0] < end:
            continue
        kept.append(h)
        end = h[0] + h[3]

    for off, lib, obj, size, labels, ver in kept:
        print("%08X %-5s %-12s %-14s %6d  %s" % (vram + off, ver, lib, obj, size,
              " ".join(l["name"] for l in labels if is_function_label(l["name"]))))

    if args.symbols:
        seen = set()
        with open(args.symbols, "w") as out:
            out.write("// PSY-Q %s library functions, from tools/psyq_match.py\n" % version)
            for off, lib, obj, size, labels, ver in kept:
                out.write("// %s %s (PSY-Q %s)\n" % (lib, obj, ver))
                for l in labels:
                    if is_function_label(l["name"]) and l["name"] not in seen:
                        seen.add(l["name"])
                        out.write("%s = 0x%08X; // type:func\n" % (l["name"], vram + off + l["offset"]))
    if args.segments:
        with open(args.segments, "w") as out:
            for off, lib, obj, size, labels, ver in kept:
                name = "psyq/%s/%s" % (lib.split(".")[0].lower(), obj.split(".")[0].lower())
                out.write("      - [0x%X, asm, %s] # %d bytes, PSY-Q %s\n" % (HEADER + off, name, size, ver))


if __name__ == "__main__":
    main()
