"""Transfer names from the (debug-build) link map onto the retail DOL.

The shipped debugInfoS.MAP describes a different build than the retail
main.dol, so addresses do not line up.  Object files that were linked
unchanged (SDK libraries, runtime, much of JSystem) keep their exact internal
layout though, and objects keep their relative link order.

Pass 1  every object: find retail bases where the object's function offsets
        coincide with REA/Ghidra procedure starts (strict: object end is a
        boundary and no foreign procedure starts inside).  Unique => anchor.
Pass 2  drop anchors that break link order (longest increasing subsequence).
Pass 3  repeat until stable: for each unanchored object, restrict the search
        to the gap between its anchored neighbours; accept a unique strict
        candidate, else a unique relaxed one (extra Ghidra splits allowed).
Pass 4  a gap holding exactly one unanchored object whose total size equals
        the gap is matched outright (yields function starts Ghidra missed).

Inputs : build/rea_procedures.json (REA list_procedures), build/symbols.json
Outputs: build/matched_symbols.json  [{addr,name,size,obj,how,known}]
         build/seeds.tsv             REA_GHIDRA_SEED_FILE (function starts + names)
"""
import bisect
import collections
import json
import os
import re
import sys

TEXT_LO, TEXT_HI = 0x80003100, 0x802D84A0


class Obj:
    def __init__(self, idx, key, funcs):
        self.idx = idx
        self.sec, self.name = key
        self.funcs = funcs
        base = funcs[0]["addr"]
        self.offs = [f["addr"] - base for f in funcs]
        self.offset_set = set(self.offs)
        self.total = funcs[-1]["addr"] + funcs[-1]["size"] - base
        self.base = None   # retail base once anchored
        self.how = None


def candidates(o, procs, pset, lo, hi, strict):
    n = len(o.funcs)
    need = max(2, int(n * 0.6 + 0.5)) if n >= 2 else 1
    out = []
    i = bisect.bisect_left(procs, lo)
    while i < len(procs) and procs[i] + o.total <= hi:
        b = procs[i]
        i += 1
        hits = miss = 0
        for off in o.offs:
            if b + off in pset:
                hits += 1
            else:
                miss += 1
                if miss > n - need:
                    break
        if hits < need:
            continue
        end_ok = (b + o.total) in pset or b + o.total >= TEXT_HI - 0x40
        if strict:
            inner = procs[bisect.bisect_right(procs, b):bisect.bisect_left(procs, b + o.total)]
            if not end_ok or any((p - b) not in o.offset_set for p in inner):
                continue
        elif not end_ok and hits < n:
            continue
        out.append((hits, b))
    return out


def lis_filter(objs):
    """Keep the largest set of anchors whose retail order matches map order."""
    anch = [o for o in objs if o.base is not None]
    tails, prev, tail_idx = [], [None] * len(anch), []
    for k, o in enumerate(anch):
        j = bisect.bisect_left(tails, o.base)
        if j == len(tails):
            tails.append(o.base)
            tail_idx.append(k)
        else:
            tails[j] = o.base
            tail_idx[j] = k
        prev[k] = tail_idx[j - 1] if j > 0 else None
    keep = set()
    k = tail_idx[-1] if tail_idx else None
    while k is not None:
        keep.add(k)
        k = prev[k]
    dropped = 0
    for k, o in enumerate(anch):
        if k not in keep:
            o.base = None
            o.how = None
            dropped += 1
    return dropped


def window(objs, i):
    lo, hi = TEXT_LO, TEXT_HI
    for j in range(i - 1, -1, -1):
        if objs[j].base is not None:
            lo = objs[j].base + objs[j].total
            break
    for j in range(i + 1, len(objs)):
        if objs[j].base is not None:
            hi = objs[j].base
            break
    return lo, hi


def main():
    procs = {int(p["address"], 16) for p in json.load(open("build/rea_procedures.json"))["normalized_result"]}
    if os.path.exists("build/recomp/functions.json"):
        # the recompiler's partition is a superset of Ghidra's procedures
        procs |= {f["start"] for f in json.load(open("build/recomp/functions.json"))}
    procs = sorted(procs)
    pset = set(procs)
    syms = [s for s in json.load(open("build/symbols.json"))
            if s["kind"] == "func" and s["section"] in (".text", ".init")]
    groups = collections.OrderedDict()
    for s in sorted(syms, key=lambda s: s["addr"]):
        groups.setdefault((s["section"], s["obj"]), []).append(s)
    objs = [Obj(i, k, v) for i, (k, v) in enumerate(groups.items())]

    # pass 1
    for o in objs:
        if len(o.funcs) < 3 and o.total < 0x40:
            continue
        c = candidates(o, procs, pset, TEXT_LO, TEXT_HI, True)
        if len(c) == 1:
            o.base, o.how = c[0][1], "global"
    print("pass1 anchors:", sum(o.base is not None for o in objs))
    print("pass2 dropped out-of-order anchors:", lis_filter(objs))

    # pass 3
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for i, o in enumerate(objs):
            if o.base is not None:
                continue
            lo, hi = window(objs, i)
            if hi - lo < o.total:
                continue
            for strict in (True, False):
                c = candidates(o, procs, pset, lo, hi, strict)
                if len(c) == 1:
                    o.base, o.how = c[0][1], "window-strict" if strict else "window-relaxed"
                    changed = True
                    break
    print("pass3 rounds:", rounds, "anchors:", sum(o.base is not None for o in objs))

    # pass 4: exact-size single-object gaps
    filled = 0
    for i, o in enumerate(objs):
        if o.base is not None:
            continue
        lo, hi = window(objs, i)
        # neighbours in the same gap
        j0 = i
        while j0 > 0 and objs[j0 - 1].base is None:
            j0 -= 1
        j1 = i
        while j1 + 1 < len(objs) and objs[j1 + 1].base is None:
            j1 += 1
        if j0 == j1 and hi - lo == o.total:
            o.base, o.how = lo, "gap-exact"
            filled += 1
    print("pass4 gap fills:", filled)

    matched = []
    for o in objs:
        if o.base is None:
            continue
        for f, off in zip(o.funcs, o.offs):
            matched.append({"addr": o.base + off, "name": f["name"], "size": f["size"], "obj": o.name,
                            "how": o.how, "known": (o.base + off) in pset})
    by_addr = {}
    for m in matched:
        by_addr.setdefault(m["addr"], m)
    matched = sorted(by_addr.values(), key=lambda m: m["addr"])
    json.dump(matched, open("build/matched_symbols.json", "w"), indent=0)

    with open("build/seeds.tsv", "w") as f:
        f.write("# function starts recovered from debugInfoS.MAP layout matching\n")
        for m in matched:
            name = re.sub(r"[^A-Za-z0-9_.$@:<>,~*&\[\]-]", "_", m["name"])
            f.write("0x%08x\tfunction\t%s\n" % (m["addr"], name))

    new = sum(1 for m in matched if not m["known"])
    print("matched functions: %d (new starts: %d) of %d map functions; objects %d/%d"
          % (len(matched), new, len(syms), sum(o.base is not None for o in objs), len(objs)))
    libs = collections.Counter(m["obj"].split()[0] for m in matched)
    tot = collections.Counter(s["obj"].split()[0] for s in syms)
    for lib, c in tot.most_common(40):
        print("  %-30s %5d / %5d" % (lib, libs.get(lib, 0), c))


if __name__ == "__main__":
    sys.exit(main())
