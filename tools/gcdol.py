"""GameCube DOL / CodeWarrior MAP utilities.

Usage:
  python gcdol.py info  <main.dol>
  python gcdol.py syms  <main.dol> <debugInfoS.MAP> <out.json>
  python gcdol.py elf   <main.dol> <debugInfoS.MAP> <out.elf>
"""
import json
import re
import struct
import sys
from dataclasses import dataclass, field


# --------------------------------------------------------------------------- DOL

@dataclass
class DolSection:
    kind: str          # "text" or "data"
    index: int
    offset: int
    addr: int
    size: int


@dataclass
class Dol:
    data: bytes
    sections: list
    bss_addr: int
    bss_size: int
    entry: int

    def read(self, addr, size):
        for s in self.sections:
            if s.addr <= addr and addr + size <= s.addr + s.size:
                o = s.offset + (addr - s.addr)
                return self.data[o:o + size]
        return None

    def section_for(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.addr + s.size:
                return s
        return None


def load_dol(path):
    data = open(path, "rb").read()
    offs = struct.unpack_from(">18I", data, 0x00)
    addrs = struct.unpack_from(">18I", data, 0x48)
    sizes = struct.unpack_from(">18I", data, 0x90)
    bss_addr, bss_size, entry = struct.unpack_from(">3I", data, 0xD8)
    secs = []
    for i in range(18):
        if sizes[i] == 0:
            continue
        kind = "text" if i < 7 else "data"
        secs.append(DolSection(kind, i if i < 7 else i - 7, offs[i], addrs[i], sizes[i]))
    return Dol(data, secs, bss_addr, bss_size, entry)


# --------------------------------------------------------------------------- MAP

@dataclass
class MapSection:
    name: str
    addr: int
    size: int
    file_off: int


@dataclass
class MapSymbol:
    name: str
    addr: int
    size: int
    section: str
    obj: str
    kind: str = "notype"   # func / object / notype / section


@dataclass
class LinkMap:
    sections: dict = field(default_factory=dict)
    symbols: list = field(default_factory=list)
    linker_syms: dict = field(default_factory=dict)


_LAYOUT_RE = re.compile(r"^\s+([0-9a-f]{8})\s+([0-9a-f]{6,8})\s+([0-9a-f]{8})\s+(?:[0-9a-f]{8}\s+)?(\d+)\s+(\S+)\s*\t?(.*)$")
_TREE_RE = re.compile(r"^\s*\d+\]\s+(\S+)\s+\((\w+),(\w+)\)\s+found in\s+(.*)$")
_MEMMAP_RE = re.compile(r"^\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s*$")
_LINKSYM_RE = re.compile(r"^\s+(\S+)\s+([0-9a-f]{8})\s*$")


def load_map(path):
    lm = LinkMap()
    kinds = {}
    cur_sec = None
    mode = None
    for line in open(path, "r", encoding="latin-1"):
        line = line.rstrip("\r\n")
        m = re.match(r"^(\S+) section layout$", line)
        if m:
            cur_sec = m.group(1)
            mode = "layout"
            continue
        if line.startswith("Memory map:"):
            mode = "memmap"
            continue
        if line.startswith("Linker generated symbols:"):
            mode = "linksyms"
            continue
        if mode is None:
            t = _TREE_RE.match(line)
            if t:
                kinds.setdefault(t.group(1), t.group(2))
            continue
        if mode == "layout":
            m = _LAYOUT_RE.match(line)
            if m:
                _, size, vaddr, level, name, obj = m.groups()
                size = int(size, 16)
                vaddr = int(vaddr, 16)
                obj = obj.strip()
                if level == "1":
                    continue  # per-object section header line
                lm.symbols.append(MapSymbol(name, vaddr, size, cur_sec, obj))
        elif mode == "memmap":
            m = _MEMMAP_RE.match(line)
            if m:
                name, addr, size, foff = m.groups()
                lm.sections[name] = MapSection(name, int(addr, 16), int(size, 16), int(foff, 16))
        elif mode == "linksyms":
            m = _LINKSYM_RE.match(line)
            if m:
                lm.linker_syms[m.group(1)] = int(m.group(2), 16)
    code_secs = {".init", ".text"}
    for s in lm.symbols:
        k = kinds.get(s.name)
        if k is None:
            k = "func" if s.section in code_secs else "object"
        s.kind = k
    return lm


def dol_layout(dol):
    """Build a LinkMap describing the DOL's own sections (no symbols).

    Retail CodeWarrior DOLs use a fixed section order: text0=.init, text1=.text,
    data0..=extab, extabindex, .ctors, .dtors, then the remaining data sections;
    the small-data sections (.sdata/.sdata2) are placed inside the bss range.
    """
    lm = LinkMap()
    text = [s for s in dol.sections if s.kind == "text"]
    data = [s for s in dol.sections if s.kind == "data"]
    tnames = [".init", ".text"] + [".text%d" % i for i in range(2, 7)]
    for s, n in zip(text, tnames):
        lm.sections[n] = MapSection(n, s.addr, s.size, s.offset)
    bss_end = dol.bss_addr + dol.bss_size
    inside = [s for s in data if dol.bss_addr <= s.addr < bss_end]
    outside = [s for s in data if s not in inside]
    dnames = ["extab", "extabindex", ".ctors", ".dtors"]
    rest = outside[len(dnames):]
    # remaining big data sections: [.BINARY?] .rodata .data
    rnames = [".rodata", ".data"] if len(rest) == 2 else [".BINARY", ".rodata", ".data"] if len(rest) == 3 else \
        [".data%d" % i for i in range(len(rest))]
    for s, n in zip(outside, dnames + rnames):
        lm.sections[n] = MapSection(n, s.addr, s.size, s.offset)
    for s, n in zip(inside, [".sdata", ".sdata2"]):
        lm.sections[n] = MapSection(n, s.addr, s.size, s.offset)
    # bss pieces between the small-data sections
    cur = dol.bss_addr
    for n, s in zip([".bss", ".sbss", ".sbss2"], inside + [None]):
        end = s.addr if s else bss_end
        if end > cur:
            lm.sections[n] = MapSection(n, cur, end - cur, 0)
        if s:
            cur = s.addr + s.size
    lm.linker_syms["__start"] = dol.entry
    return lm


# --------------------------------------------------------------------------- checks

def verify(dol, lm):
    """Return list of (section, ok, message) comparing the DOL to the MAP."""
    out = []
    for name, ms in lm.sections.items():
        if ms.size == 0 or ms.addr < 0x80000000:
            continue
        if name in (".bss", ".sbss", ".sbss2"):
            ok = dol.bss_addr <= ms.addr and ms.addr + ms.size <= dol.bss_addr + dol.bss_size
            out.append((name, ok, "within DOL bss" if ok else "outside DOL bss"))
            continue
        ds = dol.section_for(ms.addr)
        if ds is None:
            out.append((name, False, "no DOL section at %08x" % ms.addr))
            continue
        ok = ds.addr == ms.addr and ds.size >= ms.size
        out.append((name, ok, "DOL %s%d @%08x size %x vs map size %x" % (ds.kind, ds.index, ds.addr, ds.size, ms.size)))
    return out


# --------------------------------------------------------------------------- ELF

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOBITS = 1, 2, 3, 8
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4


class StrTab:
    def __init__(self):
        self.buf = bytearray(b"\0")
        self.idx = {"": 0}

    def add(self, s):
        if s not in self.idx:
            self.idx[s] = len(self.buf)
            self.buf += s.encode("latin-1") + b"\0"
        return self.idx[s]


def write_elf(dol, lm, out_path):
    order = [".init", "extab", "extabindex", ".text", ".ctors", ".dtors", ".BINARY",
             ".rodata", ".data", ".bss", ".sdata", ".sbss", ".sdata2", ".sbss2"]
    nobits = {".bss", ".sbss", ".sbss2"}
    execs = {".init", ".text"}
    shstr = StrTab()
    strtab = StrTab()
    body = bytearray()
    hdr_size = 0x34
    secs = []  # (name, type, flags, addr, off, size, align)
    off = 0x1000
    body_base = off
    for name in order:
        ms = lm.sections.get(name)
        if ms is None or ms.size == 0:
            continue
        if name in nobits:
            secs.append((name, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, ms.addr, off, ms.size, 32))
            continue
        raw = dol.read(ms.addr, ms.size)
        if raw is None:
            raise SystemExit("section %s not found in DOL" % name)
        flags = SHF_ALLOC | (SHF_EXEC if name in execs else SHF_WRITE if name in (".data", ".sdata", ".ctors", ".dtors", ".bss") else 0)
        pad = (-off) % 32
        body += b"\0" * pad
        off += pad
        secs.append((name, SHT_PROGBITS, flags, ms.addr, off, ms.size, 32))
        body += raw
        off += len(raw)
    sec_index = {s[0]: i + 1 for i, s in enumerate(secs)}

    # symbols
    syms = [struct.pack(">IIIBBH", 0, 0, 0, 0, 0, 0)]
    seen = set()
    for s in sorted(lm.symbols, key=lambda s: s.addr):
        if s.section not in sec_index or s.kind == "section":
            continue
        if s.name.startswith("...") or s.name.startswith("@"):
            # compiler-local data labels; keep them but namespace by object
            nm = "%s$%s" % (s.name, s.obj.split()[-1])
        else:
            nm = s.name
        key = (nm, s.addr)
        if key in seen:
            continue
        seen.add(key)
        stt = 2 if s.kind == "func" else 1 if s.kind == "object" else 0
        bind = 1  # global (keep everything visible to the importer)
        syms.append(struct.pack(">IIIBBH", strtab.add(nm), s.addr, s.size, (bind << 4) | stt, 0, sec_index[s.section]))
    for nm, addr in lm.linker_syms.items():
        syms.append(struct.pack(">IIIBBH", strtab.add(nm), addr, 0, (1 << 4) | 0, 0, 0xFFF1))
    symdata = b"".join(syms)

    pad = (-off) % 4
    body += b"\0" * pad
    off += pad
    symtab_off = off
    body += symdata
    off += len(symdata)
    strtab_off = off
    body += strtab.buf
    off += len(strtab.buf)

    all_secs = list(secs)
    all_secs.append((".symtab", SHT_SYMTAB, 0, 0, symtab_off, len(symdata), 4))
    all_secs.append((".strtab", SHT_STRTAB, 0, 0, strtab_off, len(strtab.buf), 1))
    shstrtab_idx = len(all_secs) + 1
    for s in all_secs:
        shstr.add(s[0])
    shstr.add(".shstrtab")
    shstr_off = off
    body += shstr.buf
    off += len(shstr.buf)
    pad = (-off) % 4
    body += b"\0" * pad
    off += pad
    shoff = off

    # section headers
    sh = [b"\0" * 40]
    for i, (name, typ, flags, addr, soff, size, align) in enumerate(all_secs):
        link = info = entsize = 0
        if typ == SHT_SYMTAB:
            link = len(all_secs)  # index of .strtab (1-based count includes null)
            info = 1
            entsize = 16
        sh.append(struct.pack(">10I", shstr.idx[name], typ, flags, addr, soff, size, link, info, align, entsize))
    sh.append(struct.pack(">10I", shstr.idx[".shstrtab"], SHT_STRTAB, 0, 0, shstr_off, len(shstr.buf), 0, 0, 1, 0))
    shdata = b"".join(sh)

    # program headers (one PT_LOAD per alloc section)
    loads = [s for s in secs]
    phoff = hdr_size
    ph = []
    for (name, typ, flags, addr, soff, size, align) in loads:
        pflags = 4 | (1 if flags & SHF_EXEC else 0) | (2 if flags & SHF_WRITE else 0)
        filesz = 0 if typ == SHT_NOBITS else size
        ph.append(struct.pack(">8I", 1, soff if filesz else 0, addr, addr, filesz, size, pflags, 32))
    phdata = b"".join(ph)
    assert phoff + len(phdata) <= body_base

    ehdr = bytearray(b"\x7fELF\x01\x02\x01\x00" + b"\0" * 8)
    # body offsets were computed as absolute file offsets starting at body_base
    ehdr += struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, dol.entry, phoff, shoff, 0,
                        hdr_size, 32, len(ph), 40, len(sh), shstrtab_idx)
    out = bytearray(body_base)
    out[0:len(ehdr)] = ehdr
    out[phoff:phoff + len(phdata)] = phdata
    out += body
    assert len(out) == shoff
    out += shdata
    open(out_path, "wb").write(out)
    return len(syms) - 1


# --------------------------------------------------------------------------- CLI

def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd = argv[1]
    dol = load_dol(argv[2])
    if cmd == "elfdol":
        n = write_elf(dol, dol_layout(dol), argv[3])
        print("wrote %s with %d symbols" % (argv[3], n))
        return 0
    if cmd == "info":
        print("entry %08x  bss %08x+%x" % (dol.entry, dol.bss_addr, dol.bss_size))
        for s in dol.sections:
            print("%s%d off %08x addr %08x size %08x" % (s.kind, s.index, s.offset, s.addr, s.size))
        return 0
    lm = load_map(argv[3])
    res = verify(dol, lm)
    bad = [r for r in res if not r[1]]
    for r in res:
        print("%-12s %s %s" % (r[0], "OK " if r[1] else "BAD", r[2]))
    if cmd == "syms":
        funcs = [s for s in lm.symbols if s.kind == "func"]
        json.dump([s.__dict__ for s in lm.symbols], open(argv[4], "w"), indent=0)
        print("%d symbols (%d functions)" % (len(lm.symbols), len(funcs)))
    elif cmd == "elf":
        n = write_elf(dol, lm, argv[4])
        print("wrote %s with %d symbols" % (argv[4], n))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
