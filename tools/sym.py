"""Annotate guest addresses (8xxxxxxx) in a runtime log with function names.

  python tools/sym.py [DoubleDashRedux.log] [--only hang]     (default: DoubleDashRedux.log, all lines)
  python tools/sym.py --addr 80094754 800EB794            look up single addresses

Names come from tools/recomp/functions.txt (link-map names transferred to the retail
DOL); addresses outside a matched symbol fall back to the recompiled function that
contains them (fn_XXXXXXXX from build/recomp/functable.cpp).
"""
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load():
    syms = []
    with open(os.path.join(ROOT, "tools", "recomp", "functions.txt")) as f:
        for line in f:
            s = line.split()
            if len(s) == 3 and not s[0].startswith("#"):
                syms.append((int(s[0], 16), int(s[2]), s[1]))
    syms.sort()
    funcs = []
    with open(os.path.join(ROOT, "build", "recomp", "functable.cpp")) as f:
        for m in re.finditer(r"\{0x([0-9A-Fa-f]{8}), fn_", f.read()):
            funcs.append(int(m.group(1), 16))
    funcs.sort()
    return syms, [s[0] for s in syms], funcs


SYMS, SYM_ADDRS, FUNCS = load()


def name(a):
    i = bisect.bisect_right(SYM_ADDRS, a) - 1
    if i >= 0:
        base, size, n = SYMS[i]
        if a < base + max(size, 4):
            return n if a == base else f"{n}+{a - base:X}"
    j = bisect.bisect_right(FUNCS, a) - 1
    if j >= 0 and a - FUNCS[j] < 0x10000:
        return f"fn_{FUNCS[j]:08X}" + (f"+{a - FUNCS[j]:X}" if a != FUNCS[j] else "")
    return None


ADDR = re.compile(r"\b(8[0-2][0-9A-Fa-f]{6})\b")


def annotate(line):
    def rep(m):
        a = int(m.group(1), 16)
        if not (0x80003100 <= a < 0x80300000):   # .text only
            return m.group(0)
        n = name(a)
        return f"{m.group(1)}<{n}>" if n else m.group(0)
    return ADDR.sub(rep, line)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--addr":
        for a in args[1:]:
            print(a, name(int(a, 16)))
        return
    only = None
    if "--only" in args:
        i = args.index("--only")
        only = args[i + 1]
        del args[i:i + 2]
    path = args[0] if args else os.path.join(ROOT, "DoubleDashRedux.log")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if only and not line.startswith(f"[{only}]"):
                continue
            sys.stdout.write(annotate(line))


if __name__ == "__main__":
    main()
