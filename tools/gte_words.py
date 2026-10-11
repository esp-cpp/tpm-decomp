#!/usr/bin/env python3
"""Rewrite GTE (COP2 command) instructions in splat's asm output as raw words.

rabbitizer prints COP2 commands by mnemonic (rtps, mvmva 1,0,0,3,0, ...), but
the encodings in this executable are not all canonical: the PSY-Q gtemac
macros and the hand-written asm set "don't care" bits differently (dpcs exists
as both 0x4A780010 and 0x4B780010). A mnemonic macro can only produce one of
them, so the rebuild would not match. The raw word is in spimdisasm's comment
on every line; emit that, and keep the mnemonic as a comment.

    tools/gte_words.py asm/us   (run after every split; idempotent)
"""
import os
import re
import sys

GTE = ("rtps rtpt nclip op dpcs intpl mvmva ncds cdp ncdt nccs cc ncs nct sqr "
       "dcpl dpcl dpct avsz3 avsz4 ncct gpf gpl ncdt nccs ncct").split()
LINE = re.compile(r"^(\s*/\* [0-9A-F]+ [0-9A-F]+ ([0-9A-F]{8}) \*/\s+)((?:%s)\b.*)$" % "|".join(GTE))


def le_word(hexbytes):
    b = bytes.fromhex(hexbytes)
    return int.from_bytes(b, "little")


def fix(path):
    changed = 0
    out = []
    for line in open(path):
        m = LINE.match(line.rstrip("\n"))
        if m:
            out.append("%s.word 0x%08X /* %s */\n" % (m.group(1), le_word(m.group(2)), m.group(3).strip()))
            changed += 1
        else:
            out.append(line)
    if changed:
        open(path, "w").writelines(out)
    return changed


def main():
    total = 0
    for root, _, files in os.walk(sys.argv[1]):
        for f in files:
            if f.endswith(".s"):
                total += fix(os.path.join(root, f))
    print("gte_words: %d COP2 commands as .word" % total)


if __name__ == "__main__":
    main()
