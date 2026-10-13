"""Dump REA pseudocode / assembly for functions (by address or matched name).

  python tools/rea_dump.py [-a] <addr|name> ...     -a: assembly instead of pseudocode
Output is also cached under build/rea_cache/.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import reaq  # noqa: E402

CACHE = "build/rea_cache"


def resolve(key, names):
    if key.startswith("0x") or all(ch in "0123456789abcdefABCDEF" for ch in key) and len(key) == 8:
        return int(key, 16)
    return names[key]


def text_of(r):
    if isinstance(r, str):
        return r
    n = r.get("normalized_result", r)
    if isinstance(n, str):
        return n
    for k in ("pseudocode", "pseudo_code", "code", "assembly", "text", "lines", "instructions"):
        if isinstance(n, dict) and k in n:
            v = n[k]
            if isinstance(v, list):
                return "\n".join(x if isinstance(x, str) else json.dumps(x) for x in v)
            return v
    return json.dumps(n, indent=1)


def main(argv):
    asm = False
    if argv and argv[0] == "-a":
        asm = True
        argv = argv[1:]
    names = {}
    if os.path.exists("build/matched_symbols.json"):
        for m in json.load(open("build/matched_symbols.json")):
            names.setdefault(m["name"], m["addr"])
    os.makedirs(CACHE, exist_ok=True)
    for key in argv:
        addr = resolve(key, names)
        path = os.path.join(CACHE, "%08x.%s" % (addr, "asm" if asm else "c"))
        if os.path.exists(path):
            txt = open(path, encoding="utf-8").read()
        else:
            tool = "procedure_assembly" if asm else "procedure_pseudo_code"
            txt = text_of(reaq.call(tool, {"procedure": "0x%08x" % addr}))
            open(path, "w", encoding="utf-8").write(txt)
        print("=" * 20, key, "%08x" % addr)
        print(txt)


if __name__ == "__main__":
    main(sys.argv[1:])
