#!/usr/bin/env python3
"""Static recompiler: SLUS_008.84 (MIPS R3000A) -> C.

Every function of the executable becomes a C function `void f_XXXXXXXX(void)`
operating on the guest state in `recomp/runtime` (registers in `g_cpu`, memory
through the LW/SW/... accessors). The data sections are not translated: the
runtime loads the executable into guest RAM as the PlayStation's BIOS would,
and the generated code reads it there.

The output is derived from the game, so it is generated at build time from
the user's own executable and never committed.

    tools/recomp.py <SLUS_008.84> <outdir> [--files N]

Model:
  * registers live in C locals inside a function; they are written back to
    g_cpu before every call / return / trap and reloaded after calls, so
    calls pass state through g_cpu exactly like the hardware register file;
  * branch delay slots and the R3000's load delay slot are honoured;
  * `jal` -> direct C call; `jr $ra` -> return (when $ra still holds the
    value it had on entry); `jr`/`jalr` elsewhere -> a switch over the
    address-taken labels of the function, or the runtime's dispatcher;
  * loop back-edges count down `g_cpu.budget`; at zero the runtime polls its
    hardware model (timers, CD, vblank) and delivers interrupts there.
"""
import argparse
import bisect
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# USA Rev 1 layout (see docs/LAYOUT.md)
CODE = [(0x80010000, 0x80071590), (0x800838E4, 0x8008585C)]
DATA_IN_TEXT = [(0x80057734, 0x80057738), (0x8005B678, 0x8005B67C),
                (0x8005C168, 0x8005D970),
                (0x8005F3C4, 0x8005F3D4)]  # libsnd: jump table (0x8005FE84)
DATA = [(0x80071590, 0x800838E4), (0x8008585C, 0x80086800)]
ENTRY_FILE = os.path.join(ROOT, "config", "recomp_entries.us.txt")
HLE_FILE = os.path.join(ROOT, "recomp", "hle.us.txt")
SYMBOL_FILES = [os.path.join(ROOT, "config", f)
                for f in ("symbols.us.txt", "symbols.psyq.us.txt")]

RA = 31


def in_code(a):
    if a & 3:
        return False
    if not any(lo <= a < hi for lo, hi in CODE):
        return False
    return not any(lo <= a < hi for lo, hi in DATA_IN_TEXT)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------- decoding --

class Insn:
    __slots__ = ("pc", "w", "op", "rs", "rt", "rd", "sa", "imm", "target",
                 "reads", "writes", "kind", "load")

    def __init__(self, pc, w):
        self.pc = pc
        self.w = w
        self.rs = (w >> 21) & 31
        self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31
        self.sa = (w >> 6) & 31
        self.imm = w & 0xFFFF
        self.target = None
        self.reads = set()
        self.writes = set()
        self.kind = "op"  # op, branch, jump, jal, jr, jalr, trap
        self.load = False
        self.op = None
        decode(self)


HI, LO = 32, 33
SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv",
           0x07: "srav", 0x08: "jr", 0x09: "jalr", 0x0C: "syscall",
           0x0D: "break", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo",
           0x13: "mtlo", 0x18: "mult", 0x19: "multu", 0x1A: "div",
           0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
           0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt",
           0x2B: "sltu"}
IMM = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
       0x0D: "ori", 0x0E: "xori", 0x0F: "lui"}
LOADS = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
         0x25: "lhu", 0x26: "lwr"}
STORES = {0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr"}
BRANCH = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz"}


def decode(i):
    w = i.w
    opc = w >> 26
    if opc == 0:
        f = w & 63
        name = SPECIAL.get(f)
        i.op = name
        if name is None:
            i.kind = "trap"
            i.op = "invalid"
        elif name in ("sll", "srl", "sra"):
            i.reads = {i.rt}
            i.writes = {i.rd}
        elif name in ("sllv", "srlv", "srav"):
            i.reads = {i.rt, i.rs}
            i.writes = {i.rd}
        elif name == "jr":
            i.kind = "jr"
            i.reads = {i.rs}
        elif name == "jalr":
            i.kind = "jalr"
            i.reads = {i.rs}
            i.writes = {i.rd}
        elif name in ("syscall", "break"):
            i.kind = "trap"
        elif name in ("mfhi", "mflo"):
            i.reads = {HI if name == "mfhi" else LO}
            i.writes = {i.rd}
        elif name in ("mthi", "mtlo"):
            i.reads = {i.rs}
            i.writes = {HI if name == "mthi" else LO}
        elif name in ("mult", "multu", "div", "divu"):
            i.reads = {i.rs, i.rt}
            i.writes = {HI, LO}
        else:
            i.reads = {i.rs, i.rt}
            i.writes = {i.rd}
    elif opc == 1:
        r = i.rt
        names = {0: "bltz", 1: "bgez", 0x10: "bltzal", 0x11: "bgezal"}
        i.op = names.get(r)
        if i.op is None:
            i.kind = "trap"
            i.op = "invalid"
        else:
            i.kind = "branch"
            i.reads = {i.rs}
            i.target = (i.pc + 4 + (s16(i.imm) << 2)) & 0xFFFFFFFF
            if r & 0x10:
                i.writes = {RA}
    elif opc in (2, 3):
        i.op = "j" if opc == 2 else "jal"
        i.kind = "jump" if opc == 2 else "jal"
        i.target = ((i.pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if opc == 3:
            i.writes = {RA}
    elif opc in BRANCH:
        i.op = BRANCH[opc]
        i.kind = "branch"
        i.reads = {i.rs, i.rt} if opc in (4, 5) else {i.rs}
        i.target = (i.pc + 4 + (s16(i.imm) << 2)) & 0xFFFFFFFF
    elif opc in IMM:
        i.op = IMM[opc]
        i.reads = set() if i.op == "lui" else {i.rs}
        i.writes = {i.rt}
    elif opc in LOADS:
        i.op = LOADS[opc]
        i.load = True
        i.reads = {i.rs}
        if i.op in ("lwl", "lwr"):
            i.reads.add(i.rt)
        i.writes = {i.rt}
    elif opc in STORES:
        i.op = STORES[opc]
        i.reads = {i.rs, i.rt}
    elif opc == 0x10:  # COP0
        sub = i.rs
        if sub == 0:
            i.op = "mfc0"
            i.writes = {i.rt}
            i.load = True
        elif sub == 4:
            i.op = "mtc0"
            i.reads = {i.rt}
        elif sub == 0x10 and (w & 63) == 0x10:
            i.op = "rfe"
        else:
            i.op = "invalid"
            i.kind = "trap"
    elif opc == 0x12:  # COP2 (GTE)
        sub = i.rs
        if sub & 0x10:
            i.op = "cop2"
        elif sub == 0:
            i.op = "mfc2"
            i.writes = {i.rt}
            i.load = True
        elif sub == 2:
            i.op = "cfc2"
            i.writes = {i.rt}
            i.load = True
        elif sub == 4:
            i.op = "mtc2"
            i.reads = {i.rt}
        elif sub == 6:
            i.op = "ctc2"
            i.reads = {i.rt}
        else:
            i.op = "invalid"
            i.kind = "trap"
    elif opc == 0x32:
        i.op = "lwc2"
        i.reads = {i.rs}
    elif opc == 0x3A:
        i.op = "swc2"
        i.reads = {i.rs}
    else:
        i.op = "invalid"
        i.kind = "trap"
    i.reads.discard(0)
    i.writes.discard(0)


# ------------------------------------------------------------ translation --

def R(n):
    return "0u" if n == 0 else "r%d" % n


def RS(n):
    return "(int32_t)" + R(n)


def addr(i):
    off = s16(i.imm)
    if off == 0:
        return R(i.rs)
    return "%s + %du" % (R(i.rs), off & 0xFFFFFFFF) if off > 0 else \
        "%s - %du" % (R(i.rs), -off)


# Memory access sites: the scratchpad-first accessors (LWs, ...) for the
# sites in --scratch-sites, and with --sites every access reports its
# address (SITE) so a desktop run can tell which sites those are.
SCRATCH_SITES = set()
SITE_PROFILE = False
MEM_OPS = ("lb", "lh", "lw", "lbu", "lhu", "lwl", "lwr", "lwc2", "swc2") + \
    tuple(STORES.values())


def acc(i, name):
    return name + "s" if i.pc in SCRATCH_SITES else name


def op_c(i, dst=None):
    """C for a non-control instruction. dst overrides the destination
    register name (load delay slot handling)."""
    c = op_c_(i, dst)
    if SITE_PROFILE and c and i.op in MEM_OPS:
        c = "SITE(0x%08Xu, %s); %s" % (i.pc, addr(i), c)
    return c


def op_c_(i, dst=None):
    o = i.op
    rd = dst or R(i.rd)
    rt = dst or R(i.rt)
    if o == "sll":
        if i.w == 0:
            return ""
        return "%s = %s << %d;" % (rd, R(i.rt), i.sa) if i.rd else ""
    if i.load and i.rt == 0 and o in ("lb", "lh", "lw", "lbu", "lhu"):
        return "(void)%s(%s);" % (acc(i, o.upper()), addr(i))  # I/O reads have effects
    if not (i.writes - {HI, LO}) and o not in (
            "mult", "multu", "div", "divu", "mthi", "mtlo") \
            and o not in STORES.values() and o not in (
            "mtc0", "mtc2", "ctc2", "cop2", "lwc2", "swc2", "rfe"):
        return ""  # writes $zero: a nop (loads from I/O are not elided below)
    if o == "srl":
        return "%s = %s >> %d;" % (rd, R(i.rt), i.sa)
    if o == "sra":
        return "%s = (uint32_t)(%s >> %d);" % (rd, RS(i.rt), i.sa)
    if o == "sllv":
        return "%s = %s << (%s & 31);" % (rd, R(i.rt), R(i.rs))
    if o == "srlv":
        return "%s = %s >> (%s & 31);" % (rd, R(i.rt), R(i.rs))
    if o == "srav":
        return "%s = (uint32_t)(%s >> (%s & 31));" % (rd, RS(i.rt), R(i.rs))
    if o in ("mfhi", "mflo"):
        return "%s = %s;" % (rd, "hi" if o == "mfhi" else "lo")
    if o in ("mthi", "mtlo"):
        return "%s = %s;" % ("hi" if o == "mthi" else "lo", R(i.rs))
    if o == "mult":
        return "{ int64_t m_ = (int64_t)%s * %s; lo = (uint32_t)m_; " \
               "hi = (uint32_t)(m_ >> 32); }" % (RS(i.rs), RS(i.rt))
    if o == "multu":
        return "{ uint64_t m_ = (uint64_t)%s * %s; lo = (uint32_t)m_; " \
               "hi = (uint32_t)(m_ >> 32); }" % (R(i.rs), R(i.rt))
    if o == "div":
        return "DIV(%s, %s, &hi, &lo);" % (R(i.rs), R(i.rt))
    if o == "divu":
        return "DIVU(%s, %s, &hi, &lo);" % (R(i.rs), R(i.rt))
    bin_ops = {"add": "+", "addu": "+", "sub": "-", "subu": "-", "and": "&",
               "or": "|", "xor": "^"}
    if o in bin_ops:
        if o in ("or", "addu", "add") and i.rt == 0:
            return "%s = %s;" % (rd, R(i.rs))
        return "%s = %s %s %s;" % (rd, R(i.rs), bin_ops[o], R(i.rt))
    if o == "nor":
        return "%s = ~(%s | %s);" % (rd, R(i.rs), R(i.rt))
    if o == "slt":
        return "%s = %s < %s;" % (rd, RS(i.rs), RS(i.rt))
    if o == "sltu":
        return "%s = %s < %s;" % (rd, R(i.rs), R(i.rt))
    imm = i.imm
    simm = s16(imm)
    if o in ("addi", "addiu"):
        if i.rs == 0:
            return "%s = 0x%Xu;" % (rt, simm & 0xFFFFFFFF)
        return "%s = %s + 0x%Xu;" % (rt, R(i.rs), simm & 0xFFFFFFFF)
    if o == "slti":
        return "%s = %s < %d;" % (rt, RS(i.rs), simm)
    if o == "sltiu":
        return "%s = %s < 0x%Xu;" % (rt, R(i.rs), simm & 0xFFFFFFFF)
    if o == "andi":
        return "%s = %s & 0x%Xu;" % (rt, R(i.rs), imm)
    if o == "ori":
        return "%s = %s | 0x%Xu;" % (rt, R(i.rs), imm)
    if o == "xori":
        return "%s = %s ^ 0x%Xu;" % (rt, R(i.rs), imm)
    if o == "lui":
        return "%s = 0x%Xu;" % (rt, imm << 16)
    if o in ("lb", "lh", "lw", "lbu", "lhu"):
        return "%s = %s(%s);" % (rt, acc(i, o.upper()), addr(i))
    if o in ("lwl", "lwr"):
        return "%s = %s(%s, %s);" % (rt, acc(i, o.upper()), addr(i), R(i.rt))
    if o in STORES.values():
        return "%s(%s, %s);" % (acc(i, o.upper()), addr(i), R(i.rt))
    if o == "mfc0":
        return "%s = rt_mfc0(%d);" % (rt, i.rd)
    if o == "mtc0":
        return "rt_mtc0(%d, %s);" % (i.rd, R(i.rt))
    if o == "rfe":
        return "rt_rfe();"
    if o == "mfc2":
        return "%s = GTE_DR(%d);" % (rt, i.rd)
    if o == "cfc2":
        return "%s = GTE_CR(%d);" % (rt, i.rd)
    if o == "mtc2":
        return "GTE_DW(%d, %s);" % (i.rd, R(i.rt))
    if o == "ctc2":
        return "GTE_CW(%d, %s);" % (i.rd, R(i.rt))
    if o == "cop2":
        return "GTE_CMD(0x%07Xu);" % (i.w & 0x1FFFFFF)
    if o == "lwc2":
        return "GTE_DW(%d, %s(%s));" % (i.rt, acc(i, "LW"), addr(i))
    if o == "swc2":
        return "%s(%s, GTE_DR(%d));" % (acc(i, "SW"), addr(i), i.rt)
    raise ValueError("op_c: %s at %08X" % (o, i.pc))


def cond_c(i):
    o = i.op
    if o == "beq":
        if i.rs == i.rt:
            return "1"
        return "%s == %s" % (R(i.rs), R(i.rt))
    if o == "bne":
        return "%s != %s" % (R(i.rs), R(i.rt))
    if o == "blez":
        return "%s <= 0" % RS(i.rs)
    if o == "bgtz":
        return "%s > 0" % RS(i.rs)
    if o in ("bltz", "bltzal"):
        return "%s < 0" % RS(i.rs)
    if o in ("bgez", "bgezal"):
        return "%s >= 0" % RS(i.rs)
    raise ValueError(o)


class Program:
    def __init__(self, exe):
        hdr = exe[:0x800]
        if hdr[:8] != b"PS-X EXE":
            sys.exit("not a PS-X EXE")
        self.t_addr, self.t_size = struct.unpack_from("<II", hdr, 0x18)
        self.image = exe[0x800:0x800 + self.t_size]
        self.base = self.t_addr
        self.segments = [(self.base, self.image)]

    def add_overlay(self, base, data):
        data = data + b"\0" * (-len(data) % 4)
        self.segments.append((base, data))
        CODE.append((base, base + len(data)))
        DATA.append((base, base + len(data)))

    def word(self, a):
        for base, data in self.segments:
            if base <= a < base + len(data):
                return struct.unpack_from("<I", data, a - base)[0]
        raise KeyError(hex(a))

    def valid(self, a):
        return any(b <= a < b + len(d) for b, d in self.segments)


# Code the game loads at run time: (name in the game's file table, load
# address, entry points). FMV.CPE is the movie player: 0x800119B8 loads it
# at 0x80160000 and 0x80016194 calls its entry through 0x80083F00.
OVERLAYS = [("FMV.CPE", 0x80160000, [0x80161C30])]
FILE_TABLE = 0x800715A0
DISC_FILE_SECTORS = 0x8007793C


def disc_file(prog, disc, name):
    """A file of FILE.000, by the game's own file table, from a raw image."""
    def bcd(b):
        return (b >> 4) * 10 + (b & 15)
    a = FILE_TABLE
    file000 = prog.word(DISC_FILE_SECTORS)
    while True:
        w = prog.word(a)
        size = prog.word(a + 4)
        nm = b"".join(struct.pack("<I", prog.word(a + 8 + 4 * k))
                      for k in range(4)).split(b"\0")[0]
        if not nm or not all(32 < c < 127 for c in nm):
            return None
        if nm.decode() == name:
            m, sec, f = w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF
            lba = file000 + (bcd(m) * 60 + bcd(sec)) * 75 + bcd(f) - 150
            out = bytearray()
            with open(disc, "rb") as fh:
                while len(out) < size:
                    fh.seek(lba * 2352 + 24)
                    out += fh.read(2048)
                    lba += 1
            return bytes(out[:size])
        a += 24


def load_entries(prog):
    ents = set()
    for line in open(ENTRY_FILE):
        line = line.split("#")[0].strip()
        if line:
            ents.add(int(line, 0))
    names = {}
    for f in SYMBOL_FILES:
        if not os.path.exists(f):
            continue
        for line in open(f):
            if "type:func" not in line or line.lstrip().startswith("//"):
                continue
            name, rest = line.split("=", 1)
            try:
                a = int(rest.split(";")[0].strip(), 0)
            except ValueError:
                continue
            if in_code(a):
                ents.add(a)
                names[a] = name.strip()
    return ents, names


class Insns(dict):
    reach = frozenset()


def scan(prog):
    insns = Insns()
    for lo, hi in CODE:
        for a in range(lo, hi, 4):
            if in_code(a):
                insns[a] = Insn(a, prog.word(a))
    return insns


def address_taken(prog, insns):
    """Code addresses that appear in data, or are built by lui/addiu pairs."""
    taken = set()
    for lo, hi in DATA + DATA_IN_TEXT:
        for a in range(lo, hi, 4):
            v = prog.word(a)
            if in_code(v):
                taken.add(v)
    # %hi/%lo pairs: lui rX, hi ; addiu rY, rX, lo  (within a few insns)
    addrs = sorted(insns)
    for k, a in enumerate(addrs):
        i = insns[a]
        if i.op != "lui":
            continue
        for b in addrs[k + 1:k + 8]:
            j = insns[b]
            if j.op in ("addiu", "ori") and j.rs == i.rt:
                v = (i.imm << 16) + (s16(j.imm) if j.op == "addiu" else j.imm)
                v &= 0xFFFFFFFF
                if in_code(v):
                    taken.add(v)
            if i.rt in j.writes:
                break
    return taken


class Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.labels = set()
        self.jr_targets = []
        self.entries = [start]


def reachable(insns, ents, taken):
    """Flood-fill the code from the entry points. What is never reached is
    data that happens to sit in .text (tables, strings): it is left out, so
    its accidental 'branches' do not shape the functions."""
    reach = set()
    work = sorted(e for e in ents if e in insns)
    jr_sites = set()
    while True:
        while work:
            a = work.pop()
            while a in insns and a not in reach:
                i = insns[a]
                if i.op == "invalid":
                    break
                reach.add(a)
                if i.kind == "branch":
                    if i.target in insns:
                        work.append(i.target)
                elif i.kind == "jal":
                    if i.target in insns:
                        work.append(i.target)
                        ents.add(i.target)
                elif i.kind in ("jump", "jr"):
                    if i.kind == "jump" and i.target in insns:
                        work.append(i.target)
                    if i.kind == "jr" and i.rs != RA:
                        jr_sites.add(a)
                    if a + 4 in insns:
                        reach.add(a + 4)  # the delay slot
                    break
                a += 4
        # jump tables: address-taken code near a reachable `jr reg`
        more = []
        starts = sorted(ents)
        for site in jr_sites:
            k = bisect.bisect_right(starts, site) - 1
            lo = starts[k] if k >= 0 else site
            hi = starts[k + 1] if k + 1 < len(starts) else site + 0x10000
            more += [t for t in taken if lo <= t < hi and t not in reach]
        if not more:
            return reach
        work = more


def build_functions(insns, ents, taken):
    """Regions between entry points, merged where code branches into the
    middle of a neighbour (a mis-split, or hand-written asm sharing tails):
    a merged group becomes one C function with several entry points."""
    for i in insns.values():
        if i.kind == "jal" and i.target in insns:
            ents.add(i.target)
    starts = sorted(e for e in ents if e in insns)
    regions = []
    for k, s in enumerate(starts):
        nxt = starts[k + 1] if k + 1 < len(starts) else None
        a = s
        last = s
        while a in insns and (nxt is None or a < nxt):
            if a in insns.reach:
                last = a + 4
            a += 4
        regions.append((s, max(last, s + 4)))
    n = len(regions)
    group = list(range(n))  # group[k] = first region of k's group

    def find(k):
        while group[k] != k:
            k = group[k]
        return k

    def region_of(a):
        k = bisect.bisect_right(starts, a) - 1
        return k if k >= 0 and regions[k][0] <= a < regions[k][1] else None

    changed = True
    while changed:
        changed = False
        for k, (s, e) in enumerate(regions):
            for a in range(s, e, 4):
                if a not in insns.reach:
                    continue
                i = insns[a]
                if i.kind not in ("branch", "jump") or i.target not in insns:
                    continue
                t = i.target
                r = region_of(t)
                if r is None or find(r) == find(k):
                    continue
                if t == regions[r][0]:
                    continue  # a (conditional) tail call: no merge needed
                lo, hi = min(k, r), max(k, r)
                ok = True
                if not ok:
                    print("recomp: warning: %08X -> %08X crosses a hole" % (a, t),
                          file=sys.stderr)
                    continue
                root = find(lo)
                for j in range(lo, hi + 1):
                    group[find(j)] = root
                    group[j] = root
                changed = True
    funcs = []
    k = 0
    while k < n:
        root = find(k)
        j = k
        while j + 1 < n and find(j + 1) == root:
            j += 1
        f = Func(regions[k][0], regions[j][1])
        f.entries = [regions[x][0] for x in range(k, j + 1)]
        funcs.append(f)
        k = j + 1
    for f in funcs:
        for a in range(f.start, f.end, 4):
            if a not in insns.reach:
                continue
            i = insns[a]
            if i.target is not None and f.start <= i.target < f.end and \
                    i.kind in ("branch", "jump"):
                f.labels.add(i.target)
        f.labels |= set(f.entries[1:])
        f.jr_targets = sorted(t for t in taken if f.start < t < f.end)
        f.labels |= set(f.jr_targets)
    merged = sum(1 for f in funcs if len(f.entries) > 1)
    print("recomp: %d entry points in %d functions (%d merged)" % (
        len(starts), len(funcs), merged), file=sys.stderr)
    return funcs


# ------------------------------------------------------- register traffic --
#
# A call passes the guest's registers through g_cpu. Writing all of them out
# and reading all of them back at every call (the simple model) is most of
# the generated code and much of its time. The analysis below finds, for
# every recompiled function F:
#
#   LIVEIN(F)   registers F (or something it calls) may read before writing
#               them: the caller must have them in g_cpu;
#   CLOBBER(F)  registers F may leave changed: the caller reloads them, and
#               writes them out first if it still needs its own value
#               afterwards (F changes them only on some paths);
#   EXITLIVE(F) registers F's callers may read after it returns.
#
# A register a caller has written but does not pass may hold a stale value in
# g_cpu while the callee runs; the callee does not look at it, and gives it
# back unchanged. The prologue's `sw $sN, k($sp)` of a register the function
# restores before every exit is not a use (it saves whatever is there).
# Indirect calls, BIOS calls, HLE functions and traps keep the full traffic.
# The `jr $ra` that does not return is assumed not to happen (rt_jump_ra
# reports it); a jump table may leave its function (a tail call through a
# pointer).

NREG = 34
ALL = ((1 << NREG) - 1) & ~1
SAVED = [16, 17, 18, 19, 20, 21, 22, 23, 30, 31]
SP = 29
# What native code (rt_bios.c, rt_hle.c) reads and may change: the C
# calling convention's arguments, stack, gp and ra; the BIOS setjmp also
# saves the callee-saved registers.
ABI_IN = (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 28) | (1 << 29) | \
    (1 << 31)
ABI_OUT = sum(1 << r for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                15, 24, 25, 28, 29, 31, HI, LO))
# Hand-written renderers that use $sp and $ra as data registers (so the
# save / restore pattern is not visible) but whose public entry points keep
# the calling convention: they give back $s0-$s7, $gp and $fp and do not
# read them. (Their inner entries take $s0 and temporaries as inputs.)
# Checked by --check / --poison.
ASM_ABI = (0x80052618, 0x800545B4, 0x80055148)
ASM_KEEP = (16, 17, 18, 19, 20, 21, 22, 23, 28, 30)
SETJMP_IN = sum(1 << r for r in (16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30,
                                  31))


def mask(regs):
    m = 0
    for r in regs:
        m |= 1 << r
    return m & ~1


class Unit:
    __slots__ = ("kind", "pc", "uses", "defs", "succ", "callee", "slot_uses",
                 "slot_defs", "live_in", "live_out", "exits")

    def __init__(self, kind, pc):
        self.kind = kind
        self.pc = pc
        self.uses = 0
        self.defs = 0
        self.succ = []      # unit indices
        self.exits = []     # ("ret",) / ("tail", addr) / ("all",)
        self.callee = None
        self.slot_uses = 0
        self.slot_defs = 0
        self.live_in = 0
        self.live_out = 0


class RegFlow:
    ind_abi = False

    def __init__(self, em):
        self.em = em
        self.insns = em.insns
        self.funcs = em.funcs
        self.entry_of = {}            # entry address -> Func
        for f in self.funcs:
            for e in f.entries:
                self.entry_of[e] = f
        self.units = {}               # Func start -> [Unit]
        self.at = {}                  # Func start -> {pc: unit index}
        self.livein = {}              # entry address -> mask
        self.clobber = {}             # Func start -> mask
        self.exitlive = {}            # Func start -> mask
        self.saverestore = {}         # Func start -> {reg: prologue store pc}
        self.call_la = {}             # call pc -> live after the call
        self.call_target = {}         # call pc -> callee entry
        self.direct = set()           # Func starts only ever called directly
        self.model = {}               # entry -> (livein, clobber) for native code
        self.ind_livein = 0           # any address-taken function's
        self.ind_clobber = 0
        self.ind_exitlive = 0         # what callers through pointers need
        self.call_ind = set()         # jalr pcs
        self.indirect = sorted(t for t in em.taken if t in self.entry_of)

    def known(self, t):
        """A function entry the analysis has a summary for."""
        return t in self.model or t in self.entry_of

    def clobber_of(self, t):
        if t in self.model:
            return self.model[t][1]
        if t not in self.entry_of:
            return ALL
        m = self.clobber.get(t, 0)
        if t in self.em.wrap:
            m |= ABI_OUT  # native code around the original
        return m

    def bios_stub(self, f):
        """addiu $t2, $0, 0xA0/B0/C0; jr $t2; addiu $t1, $0, N -> (table, N)"""
        w = [self.insns[a].w if a in self.insns else None
             for a in (f.start, f.start + 4, f.start + 8)]
        if len(f.entries) != 1 or w[0] not in (0x240A00A0, 0x240A00B0,
                                               0x240A00C0):
            return None
        if w[1] != 0x01400008 or w[2] is None or (w[2] >> 16) != 0x2409:
            return None
        return w[0] & 0xFF, w[2] & 0xFFFF

    # ---- units, mirroring Emitter.emit_func_body ----
    def build(self, f):
        em = self.em
        ins = [self.insns[a] for a in range(f.start, f.end, 4)
               if a in self.insns.reach]
        units = []
        at = {}
        fixups = []   # (unit, target pc) resolved once all units exist
        n = len(ins)
        k = 0

        def jump(u, t):
            if f.start <= t < f.end:
                fixups.append((u, t))
            elif t in em.starts:
                u.exits.append(("tail", t))
            else:
                u.exits.append(("all",))

        while k < n:
            i = ins[k]
            idx = len(units)
            if i.kind in ("branch", "jump", "jal", "jr", "jalr"):
                slot = ins[k + 1] if k + 1 < n else None
                u = Unit(i.kind, i.pc)
                at[i.pc] = idx
                units.append(u)
                if slot is not None:
                    u.slot_uses, u.slot_defs = self.effect(slot)
                nxt_pc = ins[k + 2].pc if k + 2 < n else None
                if i.kind == "branch":
                    u.uses = mask(i.reads)
                    cond = cond_c(i)
                    if i.op in ("bltzal", "bgezal"):
                        u.kind = "condcall"
                        u.callee = i.target
                        self.call_target[i.pc] = i.target
                        fixups.append((u, nxt_pc))
                    else:
                        jump(u, i.target)
                        if cond != "1":
                            fixups.append((u, nxt_pc))
                elif i.kind == "jump":
                    jump(u, i.target)
                elif i.kind == "jal":
                    u.callee = i.target if i.target in em.starts else None
                    self.call_target[i.pc] = i.target
                    fixups.append((u, nxt_pc))
                elif i.kind == "jalr":
                    u.uses = mask(i.reads)
                    u.defs = mask(i.writes)
                    self.call_ind.add(i.pc)
                    fixups.append((u, nxt_pc))
                elif i.kind == "jr":
                    u.uses = mask(i.reads)
                    if i.rs == RA:
                        u.kind = "ret"
                        u.exits.append(("ret",))
                    else:
                        # a jump table may hold other functions too (tail
                        # calls): the analysis takes those as calls through
                        # a pointer
                        u.exits.append(("ind",))
                    for t in f.jr_targets:
                        fixups.append((u, t))
                if slot is not None and slot.pc in f.labels:
                    # the slot's own copy, for jumps straight into it
                    s = Unit("op", slot.pc)
                    s.uses, s.defs = self.effect(slot)
                    at[slot.pc] = len(units)
                    units.append(s)
                    fixups.append((s, nxt_pc))
                k += 2
                continue
            nxt = ins[k + 1] if k + 1 < n else None
            u = Unit("op", i.pc)
            at[i.pc] = idx
            units.append(u)
            if i.load and nxt is not None and nxt.kind not in (
                    "branch", "jump", "jal", "jr", "jalr", "trap") and \
                    em.load_hazard(i, nxt) and nxt.pc not in f.labels:
                u1, d1 = self.effect(i)
                u2, d2 = self.effect(nxt)
                u.uses = u1 | u2
                u.defs = d1 | d2
                k += 2
            else:
                u.uses, u.defs = self.effect(i)
                if i.kind == "trap" and i.op == "invalid":
                    u.kind = "dead"
                k += 1
            if u.kind != "dead":
                if k < n:
                    fixups.append((u, ins[k].pc))
        # the end: falls into the next function, unless it ended in a jump
        last = ins[-1] if ins else None
        ends = last is not None and last.kind in ("jump", "jr")
        if n >= 2 and ins[-2].kind in ("jump", "jr"):
            ends = True
        if n >= 2 and ins[-2].kind == "branch" and ins[-2].op == "beq" and \
                ins[-2].rs == ins[-2].rt:
            ends = True
        for u, t in fixups:
            if t is None:
                if not ends:
                    if f.end in em.starts:
                        u.exits.append(("tail", f.end))
                    else:
                        u.exits.append(("all",))
                continue
            if t not in at:
                u.exits.append(("all",))  # not expected: be safe
                continue
            u.succ.append(at[t])
        self.units[f.start] = units
        self.at[f.start] = at

    def effect(self, i):
        """(uses, must-defs) of one non-control instruction."""
        if i.kind == "trap":
            if i.op == "syscall":
                return mask([4]), 0
            return 0, 0
        return mask(i.reads), mask(i.writes)

    # ---- save / restore of callee-saved registers (single-entry only) ----
    def reach_from(self, f, e):
        """Indices of f's units reachable from entry e."""
        units = self.units[f.start]
        at = self.at[f.start]
        if e not in at:
            return set(range(len(units)))
        seen = {at[e]}
        work = [at[e]]
        while work:
            x = work.pop()
            for y in units[x].succ:
                if y not in seen:
                    seen.add(y)
                    work.append(y)
        return seen

    def find_saverestore(self, f, e, reach):
        """{reg: prologue store pc} for the registers entry e saves in its
        prologue and every exit reachable from it restores."""
        units = self.units[f.start]
        insns = self.insns
        labels = f.labels
        # the prologue: straight-line from the entry
        slots = {}
        store_pc = {}
        written = set()
        delta = 0
        a = e
        in_slot = False
        while a in insns.reach and a < f.end:
            i = insns[a]
            if a != e and a in labels:
                break
            if i.kind != "op":
                if in_slot:
                    break
                # the delay slot runs on both paths: GCC puts saves there
                in_slot = True
                a += 4
                continue
            if i.op in ("addiu", "addi") and i.rs == SP and i.rt == SP:
                delta += s16(i.imm)
            elif SP in i.writes:
                break
            elif i.op == "sw" and i.rs == SP and i.rt in SAVED and \
                    i.rt not in written and i.rt not in slots:
                slots[i.rt] = delta + s16(i.imm)
                store_pc[i.rt] = a
            written |= i.writes
            a += 4
            if in_slot:
                break
        if not slots:
            return {}
        # every exit restores the register from the same slot, straight-line
        ok = dict(slots)
        for x in reach:
            u = units[x]
            if not u.exits:
                continue
            if any(e[0] == "all" for e in u.exits):
                return {}
            if u.kind not in ("ret", "jump", "branch", "op", "jr"):
                return {}
            # walk back from the exit: the slot (if a control unit), the
            # control instruction, then plain instructions up to a label
            seq = []
            if u.kind in ("ret", "jump", "branch"):
                if u.pc + 4 in insns.reach:
                    seq.append(insns[u.pc + 4])
                seq.append(insns[u.pc])
                b = u.pc - 4
            else:
                b = u.pc
            restored = {}
            dirty = set()
            after = 0  # sp adjustments between here and the exit
            while b >= f.start and b in insns.reach:
                i = insns[b]
                if i.kind != "op" and b != u.pc:
                    break
                seq.append(i)
                if b in labels and b != u.pc:
                    break
                b -= 4
                if seq and seq[-1].pc in labels:
                    break
            for i in seq:
                if i.kind != "op":
                    continue
                if i.op in ("addiu", "addi") and i.rs == SP and i.rt == SP:
                    after += s16(i.imm)
                    continue
                if SP in i.writes:
                    break
                if i.op == "lw" and i.rs == SP and i.rt in ok and \
                        i.rt not in dirty and i.rt not in restored:
                    restored[i.rt] = s16(i.imm) - after
                dirty |= i.writes
            for r in list(ok):
                if restored.get(r) != ok[r]:
                    del ok[r]
            if not ok:
                return {}
        return {r: store_pc[r] for r in ok}

    # ---- the fixpoints ----
    def run(self):
        em = self.em
        for f in self.funcs:
            self.build(f)
        self.reach = {}
        self.sr_entry = {}
        self.skip_pcs = {}
        for f in self.funcs:
            sr = {}
            skip = {}
            for e in f.entries:
                self.reach[e] = self.reach_from(f, e)
                if e in ASM_ABI:
                    self.sr_entry[e] = {r: None for r in ASM_KEEP}
                else:
                    self.sr_entry[e] = self.find_saverestore(f, e,
                                                             self.reach[e])
                sr.update(self.sr_entry[e])
                for r, pc in self.sr_entry[e].items():
                    if pc is not None:
                        skip[pc] = r
            self.saverestore[f.start] = sr
            self.skip_pcs[f.start] = skip
        for f in self.funcs:
            stub = self.bios_stub(f)
            if stub:
                extra = SETJMP_IN if stub == (0xA0, 0x13) else 0
                self.model[f.start] = (ABI_IN | extra, ABI_OUT)
        for e in em.hle:
            if e in self.entry_of and e not in em.wrap:
                self.model[e] = (ABI_IN, ABI_OUT)
        # which functions only direct calls reach (their callers are all seen)
        called = set()
        for i in self.insns.values():
            if i.pc in self.insns.reach and i.target is not None and \
                    i.kind in ("jal", "jump", "branch"):
                called.add(i.target)
        for f in self.funcs:
            if f.start in em.hle and f.start in em.wrap:
                continue  # o_ is called from native code
            if all((e in called or e in em.taken) and e not in em.special
                   for e in f.entries):
                self.direct.add(f.start)
        self.fix_clobber()
        self.fix_live()

    def fix_clobber(self):
        for f in self.funcs:
            for e in f.entries:
                self.clobber[e] = 0
        changed = True
        rounds = 0
        while changed:
            changed = False
            rounds += 1
            for f in self.funcs:
                units = self.units[f.start]
                for ent in f.entries:
                    m = 0
                    tails = 0
                    for x in self.reach[ent]:
                        u = units[x]
                        m |= u.defs | u.slot_defs
                        if u.kind in ("jal", "condcall"):
                            m |= 1 << RA
                            m |= self.clobber_of(u.callee) \
                                if u.callee is not None else ALL
                        elif u.kind == "jalr":
                            m |= self.ind_clobber
                        elif u.kind == "op" and u.uses & (1 << 4) and \
                                self.insns[u.pc].kind == "trap":
                            m |= 1 << 2
                        for e in u.exits:
                            if e[0] == "all":
                                tails |= ALL
                            elif e[0] == "ind":
                                tails |= self.ind_clobber
                            elif e[0] == "tail":
                                tails |= self.clobber_of(e[1])
                    for r in self.sr_entry[ent]:
                        m &= ~(1 << r)
                    m |= tails
                    if ent in self.model:
                        m = self.model[ent][1]
                    if m != self.clobber[ent]:
                        self.clobber[ent] = m
                        changed = True
            ind = 0
            for t in self.indirect:
                ind |= self.clobber_of(t)
            if self.ind_abi:
                ind = ABI_OUT
            if ind != self.ind_clobber:
                self.ind_clobber = ind
                changed = True
        print("recomp: clobber sets in %d rounds" % rounds, file=sys.stderr)

    def livein_of(self, t):
        if t in self.model:
            return self.model[t][0]
        if t not in self.entry_of:
            return ALL
        m = self.livein.get(t, 0) | (1 << RA)
        if t in self.em.wrap:
            m |= ABI_IN
        return m

    def local_live(self, f, need):
        """Backward liveness over f's units. need=True: what f reads from
        g_cpu (its exits pass nothing on); need=False: what is live, with
        f's callers' needs at its exits."""
        units = self.units[f.start]
        exitlive = 0 if need else self.exitlive[f.start]
        skip_use = self.skip_pcs[f.start]
        live = [0] * len(units)
        changed = True
        while changed:
            changed = False
            for x in range(len(units) - 1, -1, -1):
                u = units[x]
                out = 0
                for s in u.succ:
                    out |= live[s]
                for e in u.exits:
                    if e[0] == "ret":
                        out |= exitlive
                    elif e[0] == "tail":
                        out |= self.livein_of(e[1]) | exitlive
                    elif e[0] == "ind":
                        out |= self.ind_livein | exitlive
                    else:
                        out |= ALL
                k = u.kind
                slot_uses = u.slot_uses
                if k != "op" and u.pc + 4 in skip_use:
                    slot_uses &= ~(1 << skip_use[u.pc + 4])  # a save in a delay slot
                if k == "op":
                    uses = u.uses
                    if u.pc in skip_use:
                        uses &= ~(1 << skip_use[u.pc])
                    li = uses | (out & ~u.defs)
                elif k in ("branch", "jump", "ret", "jr"):
                    li = u.uses | slot_uses | (out & ~u.slot_defs)
                elif k == "jal":
                    ra = 1 << RA
                    call = (self.livein_of(u.callee) & ~ra) | (out & ~ra) \
                        if u.callee is not None else ALL
                    li = slot_uses | (call & ~u.slot_defs)
                elif k == "condcall":
                    ra = 1 << RA
                    call = (self.livein_of(u.callee) & ~ra) | (out & ~ra)
                    li = u.uses | slot_uses | ((call | out) & ~u.slot_defs)
                elif k == "jalr":
                    call = (self.ind_livein & ~u.defs) | (out & ~u.defs)
                    li = u.uses | slot_uses | (call & ~u.slot_defs)
                elif k == "dead":
                    li = 0
                else:
                    raise ValueError(k)
                if not need:
                    u.live_out = out
                if li != live[x]:
                    live[x] = li
                    changed = True
        return live

    def fix_live(self):
        taken_funcs = {self.entry_of[t].start for t in self.indirect}
        # Functions native code calls (the boot, interrupt entries, the
        # overlay, BIOS event handlers) return to C that restores g_cpu or
        # reads v0 / v1 at most
        native = (1 << 2) | (1 << 3)
        for f in self.funcs:
            self.exitlive[f.start] = 0 if f.start in self.direct else native
            for u in self.units[f.start]:
                u.live_in = 0
        rounds = 0
        changed = True
        while changed:
            changed = False
            rounds += 1
            for f in self.funcs:
                live = self.local_live(f, True)
                self.local_live(f, False)
                at = self.at[f.start]
                for e in f.entries:
                    li = live[at[e]] if e in at else ALL
                    if e in ASM_ABI:
                        li &= ~mask(ASM_KEEP)  # saved, not read
                    if self.livein.get(e) != li:
                        self.livein[e] = li
                        changed = True
            ind = 0
            for t in self.indirect:
                ind |= self.livein_of(t)
            if self.ind_abi:
                ind = ABI_IN
            if ind != self.ind_livein:
                self.ind_livein = ind
                changed = True
            # callers through pointers: native ones read v0 / v1
            indx = (1 << 2) | (1 << 3)
            # what callers need after the calls
            need = {}
            for f in self.funcs:
                el = self.exitlive[f.start]
                for u in self.units[f.start]:
                    if u.kind in ("jal", "condcall") and \
                            u.callee in self.entry_of:
                        g = self.entry_of[u.callee].start
                        need[g] = need.get(g, 0) | u.live_out
                    elif u.kind == "jalr":
                        indx |= u.live_out
                    for e in u.exits:
                        if e[0] == "tail" and e[1] in self.entry_of:
                            g = self.entry_of[e[1]].start
                            need[g] = need.get(g, 0) | el
                        elif e[0] == "ind":
                            indx |= el
            self.ind_exitlive = indx
            for g in taken_funcs:
                need[g] = need.get(g, 0) | indx
            for f in self.funcs:
                m = need.get(f.start, 0)
                if m | self.exitlive[f.start] != self.exitlive[f.start]:
                    self.exitlive[f.start] |= m
                    changed = True
        print("recomp: register liveness in %d rounds" % rounds, file=sys.stderr)
        for f in self.funcs:
            for u in self.units[f.start]:
                if u.kind in ("jal", "condcall", "jalr"):
                    self.call_la[u.pc] = u.live_out

    def ret_sync(self, f, written):
        """What a return must leave in g_cpu: the registers f may change that
        a caller reads afterwards, and the saved registers f restores if
        g_cpu's copy may have changed meanwhile (passed to a callee, or
        changed by one)."""
        start = f.start
        el = self.exitlive[start]
        clob = 0
        for e in f.entries:
            clob |= self.clobber_of(e)
        m = clob & el
        sr = self.saverestore[start]
        if sr:
            touched = 0
            for u in self.units[start]:
                if u.kind in ("jal", "condcall"):
                    if u.callee is None or not self.known(u.callee):
                        touched = ALL
                    else:
                        touched |= self.clobber_of(u.callee) | \
                            self.livein_of(u.callee) | \
                            (self.clobber_of(u.callee) & u.live_out)
                elif u.kind == "jalr":
                    touched |= self.ind_clobber | self.ind_livein | u.live_out
                elif u.kind == "op" and self.insns[u.pc].kind == "trap":
                    touched = ALL
                for e in u.exits:
                    if e[0] != "ret":
                        touched = ALL  # those sync everything
            for r in sr:
                if touched >> r & 1:
                    m |= 1 << r
        return m & written

    def call_sync(self, pc, written, used):
        """(out, in) masks for the call at pc."""
        if pc in self.call_ind:
            li, clob = self.ind_livein, self.ind_clobber
        else:
            t = self.call_target.get(pc)
            if t is None or not self.known(t):
                return None
            li, clob = self.livein_of(t), self.clobber_of(t)
        out = (li | (clob & self.call_la.get(pc, ALL))) & written
        out |= 1 << RA
        return out, clob & used


# ---------------------------------------------------------------- emitting --

def fname(a):
    return "f_%08X" % a


class Emitter:
    def __init__(self, prog, insns, funcs, names, taken=(), special=(),
                 poison=False, check=False):
        self.prog = prog
        self.check = check
        self.taken = set(taken)
        self.special = set(special)
        self.poison = poison
        self.flow = None
        self.insns = insns
        self.funcs = funcs
        self.starts = {e for f in funcs for e in f.entries}
        self.names = names
        self.warn = []
        self.hle = {}
        self.wrap = set()
        for line in open(HLE_FILE):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                self.hle[int(line[0], 0)] = line[1]
                if "wrap" in line[2:]:
                    self.wrap.add(int(line[0], 0))

    def emit_func(self, f):
        if f.start in self.hle and f.start in self.wrap and len(f.entries) == 1:
            # keep the original as o_XXXXXXXX for the hle_ function to call
            body = self.emit_func_body(f).replace(
                "void %s(void) {" % fname(f.start), "void o_%08X(void) {" % f.start, 1)
            return "%s\nvoid hle_%s(void);\nvoid %s(void) { hle_%s(); }" % (
                body, self.hle[f.start], fname(f.start), self.hle[f.start])
        if f.start in self.hle:
            return "void hle_%s(void);\nvoid %s(void) { hle_%s(); }" % (
                self.hle[f.start], fname(f.start), self.hle[f.start])
        return self.emit_func_body(f)

    def emit_func_body(self, f):
        ins = [self.insns[a] for a in range(f.start, f.end, 4)
               if a in self.insns.reach]
        used = set()
        written = set()
        for i in ins:
            used |= i.reads | i.writes
            written |= i.writes
            if i.kind in ("jal", "jalr") or i.op in ("bltzal", "bgezal"):
                written.add(RA)
                used.add(RA)
        used.add(RA)
        regs = sorted(r for r in used if r < 32)
        self.regs = regs
        self.written = sorted(r for r in written if r < 32)
        self.hilo = [r for r in (HI, LO) if r in used]
        self.written_mask = mask(self.written) | \
            sum(1 << r for r in self.hilo if r in written)
        self.used_mask = mask(regs) | sum(1 << r for r in self.hilo)
        self.f = f
        out = []
        nm = self.names.get(f.start)
        out.append("/* %s */" % nm if nm else "")
        multi = len(f.entries) > 1
        if multi:
            out.append("static void b_%08X(int e_) {" % f.start)
        else:
            out.append("void %s(void) {" % fname(f.start))
        decl = ", ".join("r%d = g_cpu.r[%d]" % (r, r) for r in regs)
        out.append("    uint32_t %s;" % decl)
        if HI in used or LO in used:
            out.append("    uint32_t hi = g_cpu.hi, lo = g_cpu.lo;")
        out.append("    const uint32_t ra_in = r31;")
        out.append("    (void)ra_in;")
        out.append("    RT_ENTER(0x%08Xu);" % f.start)
        if multi:
            out.append("    switch (e_) { %s}" % "".join(
                "case %d: goto L_%08X; " % (k, e)
                for k, e in enumerate(f.entries) if k))
        k = 0
        n = len(ins)
        while k < n:
            i = ins[k]
            if i.pc in f.labels:
                out.append("L_%08X:;" % i.pc)
            if i.kind in ("branch", "jump", "jal", "jr", "jalr"):
                slot = ins[k + 1] if k + 1 < n else None
                out.extend(self.control(i, slot))
                if slot is not None and slot.pc in f.labels:
                    # something jumps straight into the delay slot: give it
                    # its own sequential copy
                    out.append("    goto L_%08X_after;" % slot.pc)
                    out.append("L_%08X:;" % slot.pc)
                    out.extend(self.plain(slot, ins[k + 2] if k + 2 < n else None))
                    out.append("L_%08X_after:;" % slot.pc)
                k += 2
                continue
            nxt = ins[k + 1] if k + 1 < n else None
            if i.load and nxt is not None and nxt.kind not in (
                    "branch", "jump", "jal", "jr", "jalr", "trap") and \
                    self.load_hazard(i, nxt) and nxt.pc not in f.labels:
                out.extend(self.load_delay(i, nxt))
                k += 2
                continue
            out.extend(self.plain(i, nxt))
            k += 1
        last = ins[-1] if ins else None
        ends = last is not None and (
            (last.kind == "jump") or (last.kind == "jr"))
        if n >= 2 and ins[-2].kind in ("jump", "jr"):
            ends = True
        if n >= 2 and ins[-2].kind == "branch" and ins[-2].op == "beq" and \
                ins[-2].rs == ins[-2].rt:
            ends = True
        if not ends:
            # falls through into the next function
            if f.end in self.starts:
                out.append("    " + self.sync_out() + " %s(); return;" %
                           fname(f.end))
            else:
                out.append("    " + self.sync_out() +
                           " rt_fallthrough(0x%08Xu); return;" % f.end)
        out.append("}")
        if multi:
            for k, e in enumerate(f.entries):
                nm = self.names.get(e)
                if e in self.hle:
                    out.append("void hle_%s(void);\nvoid %s(void) { hle_%s(); }" % (
                        self.hle[e], fname(e), self.hle[e]))
                    continue
                out.append("void %s(void) { b_%08X(%d); }%s" % (
                    fname(e), f.start, k, " /* %s */" % nm if nm else ""))
        return "\n".join(out)

    def sync_out_mask(self, m):
        s = "".join("g_cpu.r[%d] = r%d; " % (r, r) for r in self.written
                    if m >> r & 1)
        for r, nm in ((HI, "hi"), (LO, "lo")):
            if r in self.hilo and m >> r & 1:
                s += "g_cpu.%s = %s; " % (nm, nm)
        return s

    def sync_in_mask(self, m):
        s = "".join("r%d = g_cpu.r[%d]; " % (r, r) for r in self.regs
                    if m >> r & 1)
        for r, nm in ((HI, "hi"), (LO, "lo")):
            if r in self.hilo and m >> r & 1:
                s += "%s = g_cpu.%s; " % (nm, nm)
        return s

    def call_syncs(self, pc):
        """(before, after) C for the call at pc: only the registers the
        callee reads, and those it may change that are still needed."""
        r = self.flow.call_sync(pc, self.written_mask, self.used_mask) \
            if self.flow else None
        if r is None:
            return self.sync_out(), self.sync_in()
        out, inn = r
        before = self.sync_out_mask(out)
        if self.check:
            if pc in self.flow.call_ind:
                clob = self.flow.ind_clobber
            else:
                clob = self.flow.clobber_of(self.flow.call_target[pc])
            keep = ALL & ~clob
            inn_c = self.sync_in_mask(inn)
            return (before + "{ Cpu s_ = g_cpu; ",
                    "rt_clobber_check(&s_, 0x%XULL, 0x%08Xu); } %s" % (keep, pc, inn_c))
        if self.poison:
            # only what this function may hand back changed: its return does
            # not rewrite the registers it preserves
            mine = 0
            for e in self.f.entries:
                mine |= self.flow.clobber_of(e)
            before += "".join("g_cpu.r[%d] = 0xDEAD%04Xu; " % (x, x)
                              for x in self.written
                              if not out >> x & 1 and x != SP and mine >> x & 1)
        return before, self.sync_in_mask(inn)

    def sync_out(self):
        s = "".join("g_cpu.r[%d] = r%d; " % (r, r) for r in self.written)
        if self.hilo:
            s += "g_cpu.hi = hi; g_cpu.lo = lo; "
        return s

    def sync_in(self):
        s = "".join("r%d = g_cpu.r[%d]; " % (r, r) for r in self.regs)
        if self.hilo:
            s += "hi = g_cpu.hi; lo = g_cpu.lo; "
        return s

    def load_hazard(self, i, nxt):
        if not i.writes:
            return False
        r = next(iter(i.writes))
        if r not in nxt.reads:
            return False
        if i.op in ("lwl", "lwr") and nxt.op in ("lwl", "lwr") and nxt.rt == i.rt:
            return False  # lwl/lwr pairs merge with the pending load
        return True

    def load_delay(self, i, nxt):
        """R3000 load delay: the next instruction still sees the old value."""
        r = next(iter(i.writes))
        out = ["    /* %08X load delay */ { uint32_t ld_;" % i.pc,
               "    /* %08X */ %s" % (i.pc, op_c(i, dst="ld_")),
               "    /* %08X */ %s" % (nxt.pc, op_c(nxt))]
        if r not in nxt.writes:
            out.append("    %s = ld_; }" % R(r))
        else:
            out.append("    }")
        return out

    def plain(self, i, nxt):
        if i.kind == "trap":
            if i.op == "syscall":
                return ["    /* %08X */ %s rt_syscall(0x%08Xu); %s" % (
                    i.pc, self.sync_out(), i.pc, self.sync_in())]
            if i.op == "break":
                return ["    /* %08X */ %s rt_break(0x%08Xu, 0x%08Xu); %s" % (
                    i.pc, self.sync_out(), i.pc, i.w, self.sync_in())]
            return ["    /* %08X */ rt_invalid(0x%08Xu, 0x%08Xu);" % (
                i.pc, i.pc, i.w)]
        c = op_c(i)
        if not c:
            return []
        return ["    /* %08X */ %s" % (i.pc, c)]

    def jump_to(self, t, src):
        """C for transferring control to t (after the delay slot)."""
        f = self.f
        if f.start <= t < f.end:
            if t not in f.labels:
                raise ValueError("unlabelled target %08X" % t)
            if t <= src:
                return "{ if (--g_cpu.budget < 0) rt_poll(); goto L_%08X; }" % t
            return "goto L_%08X;" % t
        if t in self.starts:
            return "{ %s%s(); return; }" % (self.sync_out(), fname(t))
        self.warn.append("%08X: jump to %08X, not a function" % (src, t))
        return "{ %srt_dispatch(0x%08Xu); return; }" % (self.sync_out(), t)

    def control(self, i, slot):
        out = []
        pc = i.pc
        slot_c = []
        if slot is not None:
            if slot.kind != "op" and slot.kind != "trap":
                self.warn.append("%08X: control transfer in a delay slot" % pc)
            slot_c = self.plain(slot, None)
        if i.kind == "branch":
            cond = cond_c(i)
            link = ""
            if i.op in ("bltzal", "bgezal"):
                link = "r31 = 0x%08Xu; " % (pc + 8)
            if cond == "1":
                out.append("    /* %08X */ %s" % (pc, link))
                out.extend(slot_c)
                out.append("    " + self.jump_to(i.target, pc))
            elif link:
                # a conditional call
                out.append("    /* %08X */ { int c_ = %s;" % (pc, cond))
                out.extend(slot_c)
                before, after = self.call_syncs(pc)
                out.append("    if (c_) { r31 = 0x%08Xu; %s%s(); %s} }" % (
                    pc + 8, before, fname(i.target), after))
            elif not slot_c or not (slot.writes & i.reads):
                out.extend(slot_c)
                out.append("    /* %08X */ if (%s) %s" % (
                    pc, cond, self.jump_to(i.target, pc)))
            else:
                out.append("    /* %08X */ { int c_ = %s;" % (pc, cond))
                out.extend(slot_c)
                out.append("    if (c_) %s }" % self.jump_to(i.target, pc))
            return out
        if i.kind == "jump":
            out.extend(slot_c)
            out.append("    /* %08X */ %s" % (pc, self.jump_to(i.target, pc)))
            return out
        if i.kind == "jal":
            out.extend(slot_c)
            t = i.target
            call = fname(t) + "()" if t in self.starts else \
                "rt_dispatch(0x%08Xu)" % t
            before, after = self.call_syncs(pc)
            out.append("    /* %08X */ r31 = 0x%08Xu; %s%s; %s" % (
                pc, pc + 8, before, call, after))
            return out
        if i.kind == "jalr":
            # the target register is read before the delay slot runs
            out.append("    /* %08X */ { uint32_t t_ = %s;" % (pc, R(i.rs)))
            out.extend(slot_c)
            link = "r%d = 0x%08Xu; " % (i.rd, pc + 8) if i.rd else ""
            before, after = self.call_syncs(pc)
            out.append("    %s%srt_dispatch_ind(t_); %s}" % (
                link, before, after))
            return out
        if i.kind == "jr":
            out.append("    /* %08X */ { uint32_t t_ = %s;" % (pc, R(i.rs)))
            out.extend(slot_c)
            f = self.f
            if i.rs == RA:
                ret = self.sync_out_mask(self.flow.ret_sync(f, self.written_mask)) \
                    if self.flow else self.sync_out()
                out.append("    if (t_ == ra_in) { %sreturn; }" % ret)
            cases = "".join("case 0x%08Xu: goto L_%08X; " % (t, t)
                            for t in f.jr_targets)
            if cases:
                out.append("    switch (t_) { %s}" % cases)
            how = "rt_jump_ra" if i.rs == RA else \
                "rt_jump_tbl" if f.jr_targets else "rt_jump_ind"
            # (a `jr $ra` that does not return breaks the register analysis'
            # assumptions anyway: rt_jump_ra reports it)
            sync = self.sync_out() if i.rs != RA or not self.flow else ""
            out.append("    %s%s(t_, 0x%08Xu); return; }" % (sync, how, pc))
            return out
        raise ValueError(i.kind)


HEADER = """/* Generated by tools/recomp.py from SLUS_008.84: do not edit, do not commit. */
#include "recomp_rt.h"
#include "recomp_funcs.h"
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("outdir")
    ap.add_argument("--files", type=int, default=16)
    ap.add_argument("--disc", help="raw .bin image: also recompile the overlays")
    ap.add_argument("--full-sync", action="store_true",
                    help="pass every register at every call (no analysis)")
    ap.add_argument("--check", action="store_true",
                    help="check the analysis: registers a callee is assumed "
                         "to leave alone must be unchanged in g_cpu after it")
    ap.add_argument("--poison", action="store_true",
                    help="check the analysis: registers a call does not pass "
                         "are overwritten in g_cpu before it")
    ap.add_argument("--scratch-sites",
                    help="file of access sites (hex addresses) that go to the "
                         "scratchpad: they check it first (tools/sites.py)")
    ap.add_argument("--sites", action="store_true",
                    help="every access reports its site and address (RT_SITES "
                         "builds), to measure --scratch-sites")
    args = ap.parse_args()
    global SITE_PROFILE
    SITE_PROFILE = args.sites
    if args.scratch_sites:
        for line in open(args.scratch_sites):
            line = line.split("#")[0].strip()
            if line:
                SCRATCH_SITES.add(int(line, 16))

    exe = open(args.exe, "rb").read()
    prog = Program(exe)
    overlay_entries = []
    if args.disc:
        for name, base, entries in OVERLAYS:
            data = disc_file(prog, args.disc, name)
            if data is None:
                sys.exit("recomp: %s not found on the disc" % name)
            prog.add_overlay(base, data)
            overlay_entries += entries
            print("recomp: overlay %s at %08X (%d bytes)" % (name, base, len(data)),
                  file=sys.stderr)
    insns = scan(prog)
    ents, names = load_entries(prog)
    ents.add(0x8005E210)  # __start
    ents |= set(overlay_entries)
    taken = address_taken(prog, insns)
    # setjmp (BIOS A0:13) returns a second time when the kernel "longjmps"
    # into it on an interrupt: every return point of a call to a setjmp
    # stub must be an entry
    stubs = [a for a, i in insns.items()
             if i.w == 0x240A00A0 and insns.get(a + 4) and insns[a + 4].w == 0x01400008
             and insns.get(a + 8) and insns[a + 8].w == 0x24090013]
    setjmp_points = []
    for a, i in insns.items():
        if i.kind == "jal" and i.target in stubs:
            ents.add(a + 8)
            setjmp_points.append(a + 8)
            print("recomp: setjmp return point %08X" % (a + 8), file=sys.stderr)
    # overlays have no symbol list: callbacks they take the address of are
    # functions when they start with a stack frame
    for base, data in prog.segments[1:]:
        for t in taken:
            if base <= t < base + len(data) and t in insns and \
                    (insns[t].w & 0xFFFF8000) == 0x27BD8000:
                ents.add(t)
    # address-taken code that starts right after a return is a function
    for t in taken:
        p = insns.get(t - 8)
        if p is not None and p.kind == "jr" and p.rs == RA:
            ents.add(t)
    insns.reach = reachable(insns, ents, taken)
    print("recomp: %d of %d words in .text reachable as code" % (
        len(insns.reach), len(insns)), file=sys.stderr)
    funcs = build_functions(insns, ents, taken)
    special = set(overlay_entries) | {0x8005E210} | set(setjmp_points)
    # function addresses in tables that sit in .text (some of which the
    # flood fill decoded as code)
    fstarts = {e for f in funcs for e in f.entries}
    ptrs = {i.w for i in insns.values() if i.w in fstarts}
    em = Emitter(prog, insns, funcs, names, taken | ptrs, special, args.poison,
                 args.check)
    if not args.full_sync:
        em.flow = RegFlow(em)
        em.flow.run()

    os.makedirs(args.outdir, exist_ok=True)
    bodies = [em.emit_func(f) for f in funcs]
    per = (len(bodies) + args.files - 1) // args.files
    written = []
    for n in range(args.files):
        chunk = bodies[n * per:(n + 1) * per]
        path = os.path.join(args.outdir, "recomp_%02d.c" % n)
        text = HEADER + "\n\n".join(chunk) + "\n"
        if not os.path.exists(path) or open(path).read() != text:
            open(path, "w").write(text)
        written.append(path)
    with open(os.path.join(args.outdir, "recomp_funcs.h"), "w") as h:
        h.write("/* Generated by tools/recomp.py: do not edit. */\n#pragma once\n")
        for f in funcs:
            for e in f.entries:
                h.write("void %s(void);\n" % fname(e))
    with open(os.path.join(args.outdir, "recomp_table.c"), "w") as t:
        t.write(HEADER)
        t.write("const RecompFunc g_recomp_funcs[] = {\n")
        nents = 0
        for f in funcs:
            for e in f.entries:
                t.write("    {0x%08Xu, %s},\n" % (e, fname(e)))
                nents += 1
        t.write("};\nconst unsigned g_recomp_nfuncs = %d;\n" % nents)
        t.write("const char* const g_recomp_names[][2] = {\n")
        for a in sorted(names):
            t.write("    {\"%08X\", \"%s\"},\n" % (a, names[a]))
        t.write("};\nconst unsigned g_recomp_nnames = %d;\n" % len(names))
        # the targets the register analysis assumes calls through pointers
        # reach (checked by RT_DEBUG builds)
        ind = sorted(set(em.flow.indirect)) if em.flow else []
        t.write("const uint32_t g_recomp_indirect[] = {\n")
        for a in ind:
            t.write("    0x%08Xu,\n" % a)
        t.write("    0u};\nconst unsigned g_recomp_nindirect = %d;\n" % len(ind))
    for w in em.warn:
        print("recomp: warning:", w, file=sys.stderr)
    print("recomp: %d functions, %d instructions -> %d files" % (
        len(funcs), len(insns), len(written)), file=sys.stderr)


if __name__ == "__main__":
    main()
