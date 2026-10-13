"""Teach REA to admit 32-bit PowerPC ELF targets (GameCube/Wii Gekko/Broadway).

REA's Ghidra provider is processor-agnostic once a target is admitted, but its
header parser only maps a fixed set of ELF e_machine values.  This adds
EM_PPC (20) -> "ppc" and lets the Ghidra provider accept it.  Idempotent; re-run
after `rea update`.

Usage: python3 patch_rea_ppc.py [rea-agents package dir]
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                    "/opt/node-v22.23.2-linux-x64/lib/node_modules/rea-agents") / "dist"

PATCHES = [
    ("domain/binaryTarget.js",
     '        case 183:\n            return "arm64";\n    }\n    return undefined;\n};\nconst peArchitecture',
     '        case 183:\n            return "arm64";\n        case 20: // EM_PPC (patched: GameCube/Wii)\n            return "ppc";\n    }\n    return undefined;\n};\nconst peArchitecture'),
    ("domain/binaryTargetTypes.js",
     '    "mips",\n];',
     '    "mips",\n    "ppc",\n];'),
    ("ghidra/GhidraProvider.js",
     'const SUPPORTED_ARCHITECTURES = new Set([\n    "x86",\n    "x86_64",\n    "arm",\n    "arm64",\n    "mips",\n]);',
     'const SUPPORTED_ARCHITECTURES = new Set([\n    "x86",\n    "x86_64",\n    "arm",\n    "arm64",\n    "mips",\n    "ppc",\n]);'),
]

ok = True
for rel, old, new in PATCHES:
    p = root / rel
    text = p.read_text()
    if new in text:
        print("already patched:", rel)
        continue
    if old not in text:
        print("PATTERN NOT FOUND:", rel)
        ok = False
        continue
    p.write_text(text.replace(old, new, 1))
    print("patched:", rel)
sys.exit(0 if ok else 1)
