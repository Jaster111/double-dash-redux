"""Decode every word of the DOL's code sections and report mnemonic counts."""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))
import gcdol  # noqa: E402
import ppc  # noqa: E402


def main(path):
    dol = gcdol.load_dol(path)
    cnt = collections.Counter()
    bad = []
    for s in dol.sections:
        if s.kind != "text":
            continue
        for off in range(0, s.size, 4):
            w = int.from_bytes(dol.data[s.offset + off:s.offset + off + 4], "big")
            i = ppc.decode(w, s.addr + off)
            if i is None:
                bad.append((s.addr + off, w))
                cnt["<invalid>"] += 1
            else:
                cnt[i.op + ("." if i.rc and i.op not in ("b", "bc", "bclr", "bcctr") and not i.op.endswith(".") and i.op[0] not in "lsc" else "")] += 1
    for op, c in sorted(cnt.items(), key=lambda x: -x[1]):
        print("%-12s %d" % (op, c))
    print("invalid words:", len(bad))
    for a, w in bad[:40]:
        print("  %08x %08x" % (a, w))


if __name__ == "__main__":
    main(sys.argv[1])
