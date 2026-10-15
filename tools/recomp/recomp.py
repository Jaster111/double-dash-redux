"""Static recompiler: GameCube main.dol (PowerPC Gekko) -> C++.

  python tools/recomp/recomp.py              (run from the project root)

Inputs
  orig/sys/main.dol
  build/rea_procedures.json      REA list_procedures output (Ghidra analysis)
  build/matched_symbols.json     names recovered from debugInfoS.MAP (optional)
  runtime/hle_functions.txt      "<addr|name> <hle symbol>" functions implemented natively
Outputs
  build/recomp/funcs_NNN.cpp     generated guest functions
  build/recomp/funcs.h           prototypes
  build/recomp/functable.cpp     guest address -> host function table
  build/recomp/functions.json    function list (start, end, name) for tooling
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import gcdol  # noqa: E402
import ppc  # noqa: E402

OUT = "build/recomp"
FUNCS_PER_FILE = 400

BRANCH_OPS = {"b", "bc", "bclr", "bcctr"}


# =========================================================================== analysis

class Program:
    def __init__(self, dol):
        self.dol = dol
        self.text = [(s.addr, s.addr + s.size) for s in dol.sections if s.kind == "text"]
        self.data_secs = [s for s in dol.sections if s.kind == "data"]
        self.insns = {}
        self.ptr_list = []
        self.ptr_locs = {}

    def ptrs_in(self, lo, hi):
        import bisect
        i = bisect.bisect_left(self.ptr_list, lo)
        j = bisect.bisect_left(self.ptr_list, hi)
        return self.ptr_list[i:j]

    def table_ptrs_in(self, lo, hi):
        """Data-referenced addresses in [lo, hi) that look like jump-table entries: stored next
        to another pointer into the same range.  A lone pointer (vtable slot, callback) is not."""
        out = []
        for p in self.ptrs_in(lo, hi):
            for loc in self.ptr_locs.get(p, ()):
                if any(lo <= (self.word(loc + d) or 0) < hi for d in (-4, 4)):
                    out.append(p)
                    break
        return out

    def in_text(self, a):
        return any(lo <= a < hi for lo, hi in self.text)

    def word(self, a):
        b = self.dol.read(a, 4)
        return None if b is None else struct.unpack(">I", b)[0]

    def insn(self, a):
        i = self.insns.get(a)
        if i is None:
            w = self.word(a)
            i = ppc.decode(w, a) if w is not None else None
            self.insns[a] = i
        return i


def data_code_pointers(prog):
    """Every aligned word in data sections that points at decodable code."""
    ptrs = {}
    for s in prog.data_secs:
        raw = prog.dol.data[s.offset:s.offset + s.size]
        for off in range(0, len(raw) - 3, 4):
            v = struct.unpack_from(">I", raw, off)[0]
            if v & 3 or not prog.in_text(v):
                continue
            if prog.insn(v) is None:
                continue
            ptrs.setdefault(v, []).append(s.addr + off)
    return ptrs


def find_functions(prog, seeds):
    starts = set(a for a in seeds if prog.in_text(a) and prog.insn(a) is not None)
    # direct call targets
    for lo, hi in prog.text:
        for a in range(lo, hi, 4):
            i = prog.insn(a)
            if i is not None and i.op == "b" and i.lk and prog.in_text(i.target) and i.target != a + 4:
                starts.add(i.target)
    while True:
        added = split_and_discover(prog, starts)
        if not added:
            break
        starts |= added
    return sorted(starts)


def ranges_from_starts(prog, starts):
    out = []
    ss = sorted(starts)
    for k, s in enumerate(ss):
        hi = next(h for lo, h in prog.text if lo <= s < h)
        e = ss[k + 1] if k + 1 < len(ss) and ss[k + 1] < hi else hi
        out.append((s, e))
    return out


def reach(prog, start, end, extra=frozenset()):
    """Intra-range recursive descent from the entry (`extra`: addresses outside the range that also
    belong to the function, the code caves of a patch variant).

    A reached `bctr` makes every data-referenced code address inside the range
    a jump-table target (switch statements), so those are followed as well.
    """
    seen = set()
    work = [start]
    jt_added = False
    while True:
        _reach_walk(prog, start, end, seen, work, extra)
        if jt_added or not any(prog.insn(a).op == "bcctr" and not prog.insn(a).lk for a in seen):
            return seen
        jt_added = True
        work = [t for t in prog.table_ptrs_in(start, end) if t not in seen]


def _reach_walk(prog, start, end, seen, work, extra=frozenset()):
    def inside(t):
        return start <= t < end or t in extra
    while work:
        a = work.pop()
        while inside(a) and a not in seen:
            i = prog.insn(a)
            if i is None:
                break
            seen.add(a)
            if i.op == "b":
                if i.lk:
                    a += 4
                    continue
                if inside(i.target):
                    a = i.target
                    continue
                break
            if i.op == "bc":
                if not i.lk and inside(i.target):
                    work.append(i.target)
                if (i.bo & 0x14) == 0x14:  # branch always
                    break
                a += 4
                continue
            if i.op in ("bclr", "bcctr"):
                if (i.bo & 0x14) == 0x14 and not i.lk:
                    break
                a += 4
                continue
            if i.op == "rfi":
                break
            a += 4


def split_and_discover(prog, starts):
    """Code inside a range that is unreachable from its entry starts a new function."""
    added = set()
    for s, e in ranges_from_starts(prog, starts):
        seen = reach(prog, s, e)
        # functions are contiguous: unreachable code followed by reached code of
        # the same range is dead code inside the function, not a new function
        a = max(seen) + 4 if seen else s
        while a < e:
            if a in seen:
                a += 4
                continue
            i = prog.insn(a)
            w = prog.word(a)
            if i is None or w == 0:
                a += 4
                continue
            # unreachable decodable code: a new function begins here
            added.add(a)
            break
    return added


# =========================================================================== code generation

def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


def hx(v):
    return "0x%X" % (v & 0xFFFFFFFF)


def simm(v):
    return "(u32)%d" % v if v < 0 else hx(v)


class Emitter:
    def __init__(self, prog, funcs, names, hle, jump_targets):
        self.prog = prog
        self.funcs = funcs                  # list of (start, end)
        self.fstart = set(s for s, _ in funcs)
        self.names = names
        self.hle = hle                      # addr -> hle symbol
        self.jump_targets = jump_targets    # sorted list of data-referenced code addresses
        self.warn = []
        self.alias = {}                     # patch variants: callable code caves, addr -> function name
        self._extra = frozenset()           # code caves of the function being generated

    # ---------------------------------------------------------------- helpers
    @staticmethod
    def ra0(ra):
        return "R(%d)" % ra if ra else "0"

    def ea_d(self, i):
        if i.ra == 0:
            return hx(i.imm)
        return "(R(%d) + %s)" % (i.ra, simm(i.imm)) if i.imm else "R(%d)" % i.ra

    def ea_x(self, i):
        return "(R(%d) + R(%d))" % (i.ra, i.rb) if i.ra else "R(%d)" % i.rb

    @staticmethod
    def fast(i):
        # stack (r1) and small-data (r2, r13) bases can never point at MMIO
        return i.ra in (1, 2, 13)

    def cr0(self, expr):
        return "SET_CR0(%s);" % expr

    def fname(self, a):
        return self.alias.get(a) or "fn_%08X" % a

    def call(self, target, ret):
        if target in self.fstart or target in self.alias:
            return "CALL(%s, %s);" % (self.fname(target), hx(ret))
        self.warn.append("call to non-function %08x" % target)
        return "c->lr = %s; call_indirect(c, %s);" % (hx(ret), hx(target))

    def cond(self, i):
        bo, bi = i.bo, i.bi
        parts = []
        pre = ""
        if not (bo & 4):
            pre = "c->ctr--; "
            parts.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
        if not (bo & 16):
            bit = "CRBIT(%d)" % bi
            parts.append(bit if bo & 8 else "!" + bit)
        return pre, " && ".join(parts) if parts else "1"

    # ---------------------------------------------------------------- function body
    def function(self, start, end):
        out = []
        nm = self.names.get(start)
        if start in self.hle:
            # native replacement; the original body stays available as orig_XXXXXXXX
            out.append("// %s (HLE)" % (nm or ""))
            out.append("void %s(CPU* c) { %s(c); }" % (self.fname(start), self.hle[start]))
            body = self.body(start, end, nm, "orig_%08X" % start)
            return out + body
        return self.body(start, end, nm, self.fname(start))

    def body(self, start, end, nm, cname, extra=frozenset()):
        out = []
        self._extra = extra
        seen = reach(self.prog, start, end, extra)
        labels = set()
        for a in seen:
            i = self.prog.insn(a)
            if i.op in ("b", "bc") and not i.lk and self.inside(i.target, start, end):
                labels.add(i.target)
        # jump-table targets referenced from data that fall inside this function
        jt = [t for t in self.jt_in(start, end) if t in seen]
        has_bctr = any(self.prog.insn(a).op == "bcctr" and not self.prog.insn(a).lk for a in seen)
        if has_bctr:
            labels.update(jt)
        out.append("// %08X-%08X %s" % (start, end, nm or ""))
        out.append("void %s(CPU* c) {" % cname)
        out.append("  c->pc = 0x%08X;" % start)
        if start in labels:
            out.append("L_%08X:;" % start)
            labels.discard(start)
            self._entry_label = True
        else:
            self._entry_label = False
        a = start
        prev_terminal = False
        while a < end:
            if a not in seen:
                a += 4
                prev_terminal = True
                continue
            i = self.prog.insn(a)
            if a in labels:
                out.append("L_%08X:;" % a)
            prev_terminal = False
            code = self.insn(i, a, start, end, jt if has_bctr else [])
            out.append("  " + code)
            prev_terminal = self.terminal(i)
            a += 4
        last = max((x for x in seen if start <= x < end), default=start)
        li = self.prog.insn(last)
        tail = []
        if not self.terminal(li):
            nxt = last + 4
            if li.op == "b" and li.lk and nxt not in self.fstart:
                out.append("  return; // call at end of function (noreturn?)")
            elif nxt in self.fstart:
                # also covers a call whose return address was mis-seeded as a function start
                out.append("  %s(c); return; // falls through" % self.fname(nxt))
            else:
                out.append("  rt_fatal_fallthrough(c, %s);" % hx(nxt))
        # code caves: entered and left only by branches
        for a in sorted(x for x in seen if x in extra):
            i = self.prog.insn(a)
            if a in labels:
                out.append("L_%08X:;" % a)
            out.append("  " + self.insn(i, a, start, end, []) + "  // cave")
        self._extra = frozenset()
        out.append("}")
        return out

    def inside(self, t, start, end):
        return start <= t < end or t in self._extra

    def jt_in(self, s, e):
        import bisect
        lo = bisect.bisect_left(self.jump_targets, s)
        hi = bisect.bisect_left(self.jump_targets, e)
        return self.jump_targets[lo:hi]

    @staticmethod
    def terminal(i):
        if i.op == "b" and not i.lk:
            return True
        if i.op in ("bclr", "bcctr") and (i.bo & 0x14) == 0x14 and not i.lk:
            return True
        if i.op == "bc" and (i.bo & 0x14) == 0x14 and not i.lk:
            return True
        return i.op == "rfi"

    # ---------------------------------------------------------------- branches
    def goto(self, target, a, start):
        if target <= a:
            return "{ POLL(); goto L_%08X; }" % target
        return "goto L_%08X;" % target

    def branch(self, i, a, start, end, jt):
        nxt = a + 4
        if i.op == "b":
            if i.lk:
                if i.target == nxt:
                    return "c->lr = %s; // bl next (get pc)" % hx(nxt)
                return self.call(i.target, nxt)
            if self.inside(i.target, start, end):
                return self.goto(i.target, a, start)
            if i.target in self.fstart or i.target in self.alias:
                return "%s(c); return; // tail call" % self.fname(i.target)
            self.warn.append("branch to unknown %08x from %08x" % (i.target, a))
            return "call_indirect(c, %s); return;" % hx(i.target)
        if i.op == "bc":
            pre, cond = self.cond(i)
            if i.lk:
                body = self.call(i.target, nxt) if i.target != nxt else "c->lr = %s;" % hx(nxt)
            elif self.inside(i.target, start, end):
                body = self.goto(i.target, a, start)
            elif i.target in self.fstart or i.target in self.alias:
                body = "{ %s(c); return; }" % self.fname(i.target)
            else:
                self.warn.append("cond branch to unknown %08x from %08x" % (i.target, a))
                body = "{ call_indirect(c, %s); return; }" % hx(i.target)
            return "%sif (%s) { %s }" % (pre, cond, body) if cond != "1" else pre + body
        if i.op == "bclr":
            pre, cond = self.cond(i)
            if i.lk:
                body = "{ u32 t_ = c->lr; c->lr = %s; call_indirect(c, t_); }" % hx(nxt)
            else:
                body = "return;"
            return "%sif (%s) { %s }" % (pre, cond, body) if cond != "1" else pre + body
        if i.op == "bcctr":
            pre, cond = self.cond(i)
            if i.lk:
                body = "CALL_CTR(%s);" % hx(nxt)
            else:
                cases = " ".join("case %s: goto L_%08X;" % (hx(t), t) for t in jt)
                body = "{ switch (c->ctr) { %s default: call_indirect(c, c->ctr); return; } }" % cases
            return "%sif (%s) { %s }" % (pre, cond, body) if cond != "1" else pre + body
        raise AssertionError(i.op)

    # ---------------------------------------------------------------- instructions
    def insn(self, i, a, start, end, jt):
        op = i.op
        if op in BRANCH_OPS:
            return self.branch(i, a, start, end, jt)
        m = getattr(self, "op_" + op.replace(".", "_dot"), None)
        if m is None:
            self.warn.append("unhandled %s at %08x" % (op, a))
            return "rt_unimplemented(c, %s, %s); // %s" % (hx(a), hx(i.w), op)
        return m(i, a)

    # --- integer arithmetic
    def op_addi(self, i, a):
        return "R(%d) = %s + %s;" % (i.rd, self.ra0(i.ra), simm(i.imm)) if i.ra else "R(%d) = %s;" % (i.rd, simm(i.imm))

    def op_addis(self, i, a):
        v = (i.imm << 16) & 0xFFFFFFFF
        return "R(%d) = R(%d) + %s;" % (i.rd, i.ra, hx(v)) if i.ra else "R(%d) = %s;" % (i.rd, hx(v))

    def op_addic(self, i, a):
        return "{ u32 a_ = R(%d), r_ = a_ + %s; c->ca = r_ < a_; R(%d) = r_; }" % (i.ra, simm(i.imm), i.rd)

    def op_addic_dot(self, i, a):
        return self.op_addic(i, a)[:-1] + " SET_CR0(R(%d)); }" % i.rd

    def op_subfic(self, i, a):
        return "{ u32 a_ = R(%d); c->ca = %s >= a_; R(%d) = %s - a_; }" % (i.ra, simm(i.imm), i.rd, simm(i.imm))

    def op_mulli(self, i, a):
        return "R(%d) = (u32)((s32)R(%d) * %d);" % (i.rd, i.ra, i.imm)

    def rc(self, i, code, reg):
        if i.rc:
            return code + " SET_CR0(R(%d));" % reg
        return code

    def xo_check(self, i, a):
        if i.oe:
            self.warn.append("OE form %s at %08x (overflow not modelled)" % (i.op, a))

    def op_add(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = R(%d) + R(%d);" % (i.rd, i.ra, i.rb), i.rd)

    def op_addc(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u32 a_ = R(%d), r_ = a_ + R(%d); c->ca = r_ < a_; R(%d) = r_; }" % (i.ra, i.rb, i.rd), i.rd)

    def op_adde(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)R(%d) + R(%d) + c->ca; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rb, i.rd), i.rd)

    def op_addze(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)R(%d) + c->ca; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rd), i.rd)

    def op_addme(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)R(%d) + c->ca + 0xFFFFFFFFull; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rd), i.rd)

    def op_subf(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = R(%d) - R(%d);" % (i.rd, i.rb, i.ra), i.rd)

    def op_subfc(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u32 a_ = R(%d), b_ = R(%d); c->ca = b_ >= a_; R(%d) = b_ - a_; }" % (i.ra, i.rb, i.rd), i.rd)

    def op_subfe(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)(u32)~R(%d) + R(%d) + c->ca; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rb, i.rd), i.rd)

    def op_subfze(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)(u32)~R(%d) + c->ca; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rd), i.rd)

    def op_subfme(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "{ u64 r_ = (u64)(u32)~R(%d) + c->ca + 0xFFFFFFFFull; c->ca = (u8)(r_ >> 32); R(%d) = (u32)r_; }" % (i.ra, i.rd), i.rd)

    def op_neg(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = (u32)-(s32)R(%d);" % (i.rd, i.ra), i.rd)

    def op_mullw(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = (u32)((s32)R(%d) * (s32)R(%d));" % (i.rd, i.ra, i.rb), i.rd)

    def op_mulhw(self, i, a):
        return self.rc(i, "R(%d) = (u32)(((s64)(s32)R(%d) * (s32)R(%d)) >> 32);" % (i.rd, i.ra, i.rb), i.rd)

    def op_mulhwu(self, i, a):
        return self.rc(i, "R(%d) = (u32)(((u64)R(%d) * R(%d)) >> 32);" % (i.rd, i.ra, i.rb), i.rd)

    def op_divw(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = ppc_divw(R(%d), R(%d));" % (i.rd, i.ra, i.rb), i.rd)

    def op_divwu(self, i, a):
        self.xo_check(i, a)
        return self.rc(i, "R(%d) = ppc_divwu(R(%d), R(%d));" % (i.rd, i.ra, i.rb), i.rd)

    # --- compare
    def op_cmpi(self, i, a):
        return "CMPS(%d, R(%d), %d);" % (i.crfd, i.ra, i.imm)

    def op_cmpli(self, i, a):
        return "CMPU(%d, R(%d), %s);" % (i.crfd, i.ra, hx(i.imm))

    def op_cmp(self, i, a):
        return "CMPS(%d, R(%d), R(%d));" % (i.crfd, i.ra, i.rb)

    def op_cmpl(self, i, a):
        return "CMPU(%d, R(%d), R(%d));" % (i.crfd, i.ra, i.rb)

    # --- logical (rA = rS op ...), rS is in the rd field
    def op_ori(self, i, a):
        if i.rd == i.ra and i.imm == 0:
            return ";"  # nop
        return "R(%d) = R(%d) | %s;" % (i.ra, i.rd, hx(i.imm))

    def op_oris(self, i, a):
        return "R(%d) = R(%d) | %s;" % (i.ra, i.rd, hx(i.imm << 16))

    def op_xori(self, i, a):
        return "R(%d) = R(%d) ^ %s;" % (i.ra, i.rd, hx(i.imm))

    def op_xoris(self, i, a):
        return "R(%d) = R(%d) ^ %s;" % (i.ra, i.rd, hx(i.imm << 16))

    def op_andi_dot(self, i, a):
        return "R(%d) = R(%d) & %s; SET_CR0(R(%d));" % (i.ra, i.rd, hx(i.imm), i.ra)

    def op_andis_dot(self, i, a):
        return "R(%d) = R(%d) & %s; SET_CR0(R(%d));" % (i.ra, i.rd, hx(i.imm << 16), i.ra)

    def _logic(self, i, expr):
        return self.rc(i, "R(%d) = %s;" % (i.ra, expr), i.ra)

    def op_and(self, i, a):
        return self._logic(i, "R(%d) & R(%d)" % (i.rd, i.rb))

    def op_andc(self, i, a):
        return self._logic(i, "R(%d) & ~R(%d)" % (i.rd, i.rb))

    def op_or(self, i, a):
        return self._logic(i, "R(%d) | R(%d)" % (i.rd, i.rb) if i.rd != i.rb else "R(%d)" % i.rd)

    def op_orc(self, i, a):
        return self._logic(i, "R(%d) | ~R(%d)" % (i.rd, i.rb))

    def op_xor(self, i, a):
        return self._logic(i, "R(%d) ^ R(%d)" % (i.rd, i.rb))

    def op_nor(self, i, a):
        return self._logic(i, "~(R(%d) | R(%d))" % (i.rd, i.rb))

    def op_nand(self, i, a):
        return self._logic(i, "~(R(%d) & R(%d))" % (i.rd, i.rb))

    def op_eqv(self, i, a):
        return self._logic(i, "~(R(%d) ^ R(%d))" % (i.rd, i.rb))

    def op_extsb(self, i, a):
        return self._logic(i, "(u32)(s32)(s8)R(%d)" % i.rd)

    def op_extsh(self, i, a):
        return self._logic(i, "(u32)(s32)(s16)R(%d)" % i.rd)

    def op_cntlzw(self, i, a):
        return self._logic(i, "ppc_cntlzw(R(%d))" % i.rd)

    def op_slw(self, i, a):
        return self._logic(i, "(R(%d) & 0x20) ? 0 : (R(%d) << (R(%d) & 31))" % (i.rb, i.rd, i.rb))

    def op_srw(self, i, a):
        return self._logic(i, "(R(%d) & 0x20) ? 0 : (R(%d) >> (R(%d) & 31))" % (i.rb, i.rd, i.rb))

    def op_sraw(self, i, a):
        return self.rc(i, "R(%d) = ppc_sraw(c, R(%d), R(%d));" % (i.ra, i.rd, i.rb), i.ra)

    def op_srawi(self, i, a):
        n = i.sh
        if n == 0:
            return self.rc(i, "R(%d) = R(%d); c->ca = 0;" % (i.ra, i.rd), i.ra)
        return self.rc(i, "{ s32 s_ = (s32)R(%d); c->ca = (s_ < 0) && (s_ & 0x%X); R(%d) = (u32)(s_ >> %d); }"
                       % (i.rd, (1 << n) - 1, i.ra, n), i.ra)

    def op_rlwinm(self, i, a):
        m = mask(i.mb, i.me)
        rot = "R(%d)" % i.rd if i.sh == 0 else "ROTL(R(%d), %d)" % (i.rd, i.sh)
        expr = rot if m == 0xFFFFFFFF else "%s & %s" % (rot, hx(m))
        return self._logic(i, expr)

    def op_rlwnm(self, i, a):
        m = mask(i.mb, i.me)
        return self._logic(i, "ROTL(R(%d), R(%d) & 31) & %s" % (i.rd, i.rb, hx(m)))

    def op_rlwimi(self, i, a):
        m = mask(i.mb, i.me)
        rot = "R(%d)" % i.rd if i.sh == 0 else "ROTL(R(%d), %d)" % (i.rd, i.sh)
        return self._logic(i, "(%s & %s) | (R(%d) & %s)" % (rot, hx(m), i.ra, hx(~m)))

    # --- integer loads / stores
    def _ld(self, i, a, kind, ea, update):
        fn = {"lbz": "RD8", "lhz": "RD16", "lha": "RD16S", "lwz": "RD32", "lhbr": "RD16BR", "lwbr": "RD32BR"}[kind]
        if self.fast(i) and not update and kind in ("lbz", "lhz", "lha", "lwz"):
            fn += "F"
        if update:
            return "{ u32 ea_ = %s; R(%d) = %s(ea_); R(%d) = ea_; }" % (ea, i.rd, fn, i.ra)
        return "R(%d) = %s(%s);" % (i.rd, fn, ea)

    def _st(self, i, a, kind, ea, update):
        fn = {"stb": "WR8", "sth": "WR16", "stw": "WR32", "sthbr": "WR16BR", "stwbr": "WR32BR"}[kind]
        if self.fast(i) and kind in ("stb", "sth", "stw"):
            fn += "F"
        if update:
            return "{ u32 ea_ = %s; %s(ea_, R(%d)); R(%d) = ea_; }" % (ea, fn, i.rd, i.ra)
        return "%s(%s, R(%d));" % (fn, ea, i.rd)

    def op_lbz(self, i, a): return self._ld(i, a, "lbz", self.ea_d(i), False)
    def op_lbzu(self, i, a): return self._ld(i, a, "lbz", self.ea_d(i), True)
    def op_lbzx(self, i, a): return self._ld(i, a, "lbz", self.ea_x(i), False)
    def op_lbzux(self, i, a): return self._ld(i, a, "lbz", self.ea_x(i), True)
    def op_lhz(self, i, a): return self._ld(i, a, "lhz", self.ea_d(i), False)
    def op_lhzu(self, i, a): return self._ld(i, a, "lhz", self.ea_d(i), True)
    def op_lhzx(self, i, a): return self._ld(i, a, "lhz", self.ea_x(i), False)
    def op_lhzux(self, i, a): return self._ld(i, a, "lhz", self.ea_x(i), True)
    def op_lha(self, i, a): return self._ld(i, a, "lha", self.ea_d(i), False)
    def op_lhau(self, i, a): return self._ld(i, a, "lha", self.ea_d(i), True)
    def op_lhax(self, i, a): return self._ld(i, a, "lha", self.ea_x(i), False)
    def op_lhaux(self, i, a): return self._ld(i, a, "lha", self.ea_x(i), True)
    def op_lwz(self, i, a): return self._ld(i, a, "lwz", self.ea_d(i), False)
    def op_lwzu(self, i, a): return self._ld(i, a, "lwz", self.ea_d(i), True)
    def op_lwzx(self, i, a): return self._ld(i, a, "lwz", self.ea_x(i), False)
    def op_lwzux(self, i, a): return self._ld(i, a, "lwz", self.ea_x(i), True)
    def op_lhbrx(self, i, a): return self._ld(i, a, "lhbr", self.ea_x(i), False)
    def op_lwbrx(self, i, a): return self._ld(i, a, "lwbr", self.ea_x(i), False)
    def op_lwarx(self, i, a): return self._ld(i, a, "lwz", self.ea_x(i), False) + " c->reserve = 1;"
    def op_stb(self, i, a): return self._st(i, a, "stb", self.ea_d(i), False)
    def op_stbu(self, i, a): return self._st(i, a, "stb", self.ea_d(i), True)
    def op_stbx(self, i, a): return self._st(i, a, "stb", self.ea_x(i), False)
    def op_stbux(self, i, a): return self._st(i, a, "stb", self.ea_x(i), True)
    def op_sth(self, i, a): return self._st(i, a, "sth", self.ea_d(i), False)
    def op_sthu(self, i, a): return self._st(i, a, "sth", self.ea_d(i), True)
    def op_sthx(self, i, a): return self._st(i, a, "sth", self.ea_x(i), False)
    def op_sthux(self, i, a): return self._st(i, a, "sth", self.ea_x(i), True)
    def op_stw(self, i, a): return self._st(i, a, "stw", self.ea_d(i), False)
    def op_stwu(self, i, a): return self._st(i, a, "stw", self.ea_d(i), True)
    def op_stwx(self, i, a): return self._st(i, a, "stw", self.ea_x(i), False)
    def op_stwux(self, i, a): return self._st(i, a, "stw", self.ea_x(i), True)
    def op_sthbrx(self, i, a): return self._st(i, a, "sthbr", self.ea_x(i), False)
    def op_stwbrx(self, i, a): return self._st(i, a, "stwbr", self.ea_x(i), False)

    def op_stwcx_dot(self, i, a):
        return "WR32(%s, R(%d)); c->reserve = 0; c->cr[0] = 2 | c->so;" % (self.ea_x(i), i.rd)

    def op_lmw(self, i, a):
        ea = self.ea_d(i)
        return "{ u32 ea_ = %s; %s }" % (ea, " ".join("R(%d) = RD32(ea_ + %d);" % (r, 4 * (r - i.rd)) for r in range(i.rd, 32)))

    def op_stmw(self, i, a):
        ea = self.ea_d(i)
        return "{ u32 ea_ = %s; %s }" % (ea, " ".join("WR32(ea_ + %d, R(%d));" % (4 * (r - i.rd), r) for r in range(i.rd, 32)))

    # --- float loads / stores
    def _lf(self, i, single, ea, update):
        if single:
            body = "{ double v_ = (double)RDF32(ea_); F(%d) = v_; P1(%d) = v_; }" % (i.rd, i.rd)
        else:
            body = "FU(%d) = RD64(ea_);" % i.rd
        return "{ u32 ea_ = %s; %s%s }" % (ea, body, " R(%d) = ea_;" % i.ra if update else "")

    def _sf(self, i, kind, ea, update):
        body = {"s": "WRF32(ea_, (float)F(%d));", "d": "WR64(ea_, FU(%d));", "iw": "WR32(ea_, (u32)FU(%d));"}[kind] % i.rd
        return "{ u32 ea_ = %s; %s%s }" % (ea, body, " R(%d) = ea_;" % i.ra if update else "")

    def op_lfs(self, i, a): return self._lf(i, True, self.ea_d(i), False)
    def op_lfsu(self, i, a): return self._lf(i, True, self.ea_d(i), True)
    def op_lfsx(self, i, a): return self._lf(i, True, self.ea_x(i), False)
    def op_lfsux(self, i, a): return self._lf(i, True, self.ea_x(i), True)
    def op_lfd(self, i, a): return self._lf(i, False, self.ea_d(i), False)
    def op_lfdu(self, i, a): return self._lf(i, False, self.ea_d(i), True)
    def op_lfdx(self, i, a): return self._lf(i, False, self.ea_x(i), False)
    def op_lfdux(self, i, a): return self._lf(i, False, self.ea_x(i), True)
    def op_stfs(self, i, a): return self._sf(i, "s", self.ea_d(i), False)
    def op_stfsu(self, i, a): return self._sf(i, "s", self.ea_d(i), True)
    def op_stfsx(self, i, a): return self._sf(i, "s", self.ea_x(i), False)
    def op_stfsux(self, i, a): return self._sf(i, "s", self.ea_x(i), True)
    def op_stfd(self, i, a): return self._sf(i, "d", self.ea_d(i), False)
    def op_stfdu(self, i, a): return self._sf(i, "d", self.ea_d(i), True)
    def op_stfdx(self, i, a): return self._sf(i, "d", self.ea_x(i), False)
    def op_stfdux(self, i, a): return self._sf(i, "d", self.ea_x(i), True)
    def op_stfiwx(self, i, a): return self._sf(i, "iw", self.ea_x(i), False)

    # --- floating point arithmetic. A-form: frD=rd, frA=ra, frB=rb, frC=rc_reg
    def _fd(self, i, expr):
        return "F(%d) = %s;" % (i.rd, expr)

    def _fs(self, i, expr):
        # single-precision result; Gekko also writes ps1
        return "{ double v_ = (double)(float)(%s); F(%d) = v_; P1(%d) = v_; }" % (expr, i.rd, i.rd)

    def op_fadd(self, i, a): return self._fd(i, "F(%d) + F(%d)" % (i.ra, i.rb))
    def op_fsub(self, i, a): return self._fd(i, "F(%d) - F(%d)" % (i.ra, i.rb))
    def op_fmul(self, i, a): return self._fd(i, "F(%d) * F(%d)" % (i.ra, i.rc_reg))
    def op_fdiv(self, i, a): return self._fd(i, "F(%d) / F(%d)" % (i.ra, i.rb))
    def op_fmadd(self, i, a): return self._fd(i, "F(%d) * F(%d) + F(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_fmsub(self, i, a): return self._fd(i, "F(%d) * F(%d) - F(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_fnmadd(self, i, a): return self._fd(i, "-(F(%d) * F(%d) + F(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_fnmsub(self, i, a): return self._fd(i, "-(F(%d) * F(%d) - F(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_fsel(self, i, a): return self._fd(i, "F(%d) >= 0.0 ? F(%d) : F(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_fsqrt(self, i, a): return self._fd(i, "sqrt(F(%d))" % i.rb)
    def op_frsqrte(self, i, a): return self._fd(i, "ppc_frsqrte(F(%d))" % i.rb)
    def op_fadds(self, i, a): return self._fs(i, "F(%d) + F(%d)" % (i.ra, i.rb))
    def op_fsubs(self, i, a): return self._fs(i, "F(%d) - F(%d)" % (i.ra, i.rb))
    def op_fmuls(self, i, a): return self._fs(i, "F(%d) * RC25(F(%d))" % (i.ra, i.rc_reg))
    def op_fdivs(self, i, a): return self._fs(i, "F(%d) / F(%d)" % (i.ra, i.rb))
    def op_fmadds(self, i, a): return self._fs(i, "F(%d) * RC25(F(%d)) + F(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_fmsubs(self, i, a): return self._fs(i, "F(%d) * RC25(F(%d)) - F(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_fnmadds(self, i, a): return self._fs(i, "-(F(%d) * RC25(F(%d)) + F(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_fnmsubs(self, i, a): return self._fs(i, "-(F(%d) * RC25(F(%d)) - F(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_fsqrts(self, i, a): return self._fs(i, "sqrt(F(%d))" % i.rb)
    def op_fres(self, i, a): return self._fs(i, "1.0 / F(%d)" % i.rb)
    def op_frsp(self, i, a): return self._fs(i, "F(%d)" % i.rb)
    def op_fmr(self, i, a): return "FU(%d) = FU(%d);" % (i.rd, i.rb)
    def op_fneg(self, i, a): return "FU(%d) = FU(%d) ^ 0x8000000000000000ull;" % (i.rd, i.rb)
    def op_fabs(self, i, a): return "FU(%d) = FU(%d) & 0x7FFFFFFFFFFFFFFFull;" % (i.rd, i.rb)
    def op_fnabs(self, i, a): return "FU(%d) = FU(%d) | 0x8000000000000000ull;" % (i.rd, i.rb)
    def op_fctiwz(self, i, a): return "FU(%d) = 0xFFF8000000000000ull | ppc_fctiwz(F(%d));" % (i.rd, i.rb)
    def op_fctiw(self, i, a): return "FU(%d) = 0xFFF8000000000000ull | ppc_fctiw(F(%d));" % (i.rd, i.rb)
    def op_fcmpu(self, i, a): return "FCMP(%d, F(%d), F(%d));" % (i.crfd, i.ra, i.rb)
    def op_fcmpo(self, i, a): return "FCMP(%d, F(%d), F(%d));" % (i.crfd, i.ra, i.rb)
    def op_mffs(self, i, a): return "FU(%d) = 0xFFF8000000000000ull | c->fpscr;" % i.rd
    def op_mtfsf(self, i, a): return "ppc_mtfsf(c, 0x%X, (u32)FU(%d));" % (i.fm, i.rb)
    def op_mtfsb0(self, i, a): return "c->fpscr &= ~(0x80000000u >> %d);" % i.rd
    def op_mtfsb1(self, i, a): return "c->fpscr |= (0x80000000u >> %d);" % i.rd

    def op_mtfsfi(self, i, a):
        sh = 28 - 4 * (i.rd >> 2)
        imm = (i.w >> 12) & 0xF
        return "c->fpscr = (c->fpscr & ~(0xFu << %d)) | (0x%Xu << %d);" % (sh, imm, sh)

    def op_mcrfs(self, i, a):
        return "c->cr[%d] = (c->fpscr >> %d) & 0xF;" % (i.crfd, 28 - 4 * i.crfs)

    # --- paired singles
    def _ps(self, i, e0, e1):
        return "{ double a0_ = %s, a1_ = %s; F(%d) = a0_; P1(%d) = a1_; }" % (e0, e1, i.rd, i.rd)

    def _psr(self, i, e0, e1):
        return self._ps(i, "(double)(float)(%s)" % e0, "(double)(float)(%s)" % e1)

    def op_ps_add(self, i, a): return self._psr(i, "F(%d) + F(%d)" % (i.ra, i.rb), "P1(%d) + P1(%d)" % (i.ra, i.rb))
    def op_ps_sub(self, i, a): return self._psr(i, "F(%d) - F(%d)" % (i.ra, i.rb), "P1(%d) - P1(%d)" % (i.ra, i.rb))
    def op_ps_mul(self, i, a): return self._psr(i, "F(%d) * RC25(F(%d))" % (i.ra, i.rc_reg), "P1(%d) * RC25(P1(%d))" % (i.ra, i.rc_reg))
    def op_ps_div(self, i, a): return self._psr(i, "F(%d) / F(%d)" % (i.ra, i.rb), "P1(%d) / P1(%d)" % (i.ra, i.rb))
    def op_ps_madd(self, i, a):
        return self._psr(i, "F(%d) * RC25(F(%d)) + F(%d)" % (i.ra, i.rc_reg, i.rb), "P1(%d) * RC25(P1(%d)) + P1(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_ps_msub(self, i, a):
        return self._psr(i, "F(%d) * RC25(F(%d)) - F(%d)" % (i.ra, i.rc_reg, i.rb), "P1(%d) * RC25(P1(%d)) - P1(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_ps_nmadd(self, i, a):
        return self._psr(i, "-(F(%d) * RC25(F(%d)) + F(%d))" % (i.ra, i.rc_reg, i.rb), "-(P1(%d) * RC25(P1(%d)) + P1(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_ps_nmsub(self, i, a):
        return self._psr(i, "-(F(%d) * RC25(F(%d)) - F(%d))" % (i.ra, i.rc_reg, i.rb), "-(P1(%d) * RC25(P1(%d)) - P1(%d))" % (i.ra, i.rc_reg, i.rb))
    def op_ps_muls0(self, i, a): return self._psr(i, "F(%d) * RC25(F(%d))" % (i.ra, i.rc_reg), "P1(%d) * RC25(F(%d))" % (i.ra, i.rc_reg))
    def op_ps_muls1(self, i, a): return self._psr(i, "F(%d) * RC25(P1(%d))" % (i.ra, i.rc_reg), "P1(%d) * RC25(P1(%d))" % (i.ra, i.rc_reg))
    def op_ps_madds0(self, i, a):
        return self._psr(i, "F(%d) * RC25(F(%d)) + F(%d)" % (i.ra, i.rc_reg, i.rb), "P1(%d) * RC25(F(%d)) + P1(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_ps_madds1(self, i, a):
        return self._psr(i, "F(%d) * RC25(P1(%d)) + F(%d)" % (i.ra, i.rc_reg, i.rb), "P1(%d) * RC25(P1(%d)) + P1(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_ps_sum0(self, i, a): return self._ps(i, "(double)(float)(F(%d) + P1(%d))" % (i.ra, i.rb), "P1(%d)" % i.rc_reg)
    def op_ps_sum1(self, i, a): return self._ps(i, "F(%d)" % i.rc_reg, "(double)(float)(F(%d) + P1(%d))" % (i.ra, i.rb))
    def op_ps_sel(self, i, a):
        return self._ps(i, "F(%d) >= 0.0 ? F(%d) : F(%d)" % (i.ra, i.rc_reg, i.rb), "P1(%d) >= 0.0 ? P1(%d) : P1(%d)" % (i.ra, i.rc_reg, i.rb))
    def op_ps_res(self, i, a): return self._psr(i, "1.0 / F(%d)" % i.rb, "1.0 / P1(%d)" % i.rb)
    def op_ps_rsqrte(self, i, a): return self._psr(i, "ppc_frsqrte(F(%d))" % i.rb, "ppc_frsqrte(P1(%d))" % i.rb)
    def op_ps_neg(self, i, a): return self._ps(i, "-F(%d)" % i.rb, "-P1(%d)" % i.rb)
    def op_ps_abs(self, i, a): return self._ps(i, "fabs(F(%d))" % i.rb, "fabs(P1(%d))" % i.rb)
    def op_ps_nabs(self, i, a): return self._ps(i, "-fabs(F(%d))" % i.rb, "-fabs(P1(%d))" % i.rb)
    def op_ps_mr(self, i, a): return self._ps(i, "F(%d)" % i.rb, "P1(%d)" % i.rb)
    def op_ps_merge00(self, i, a): return self._ps(i, "F(%d)" % i.ra, "F(%d)" % i.rb)
    def op_ps_merge01(self, i, a): return self._ps(i, "F(%d)" % i.ra, "P1(%d)" % i.rb)
    def op_ps_merge10(self, i, a): return self._ps(i, "P1(%d)" % i.ra, "F(%d)" % i.rb)
    def op_ps_merge11(self, i, a): return self._ps(i, "P1(%d)" % i.ra, "P1(%d)" % i.rb)
    def op_ps_cmpu0(self, i, a): return "FCMP(%d, F(%d), F(%d));" % (i.crfd, i.ra, i.rb)
    def op_ps_cmpo0(self, i, a): return "FCMP(%d, F(%d), F(%d));" % (i.crfd, i.ra, i.rb)
    def op_ps_cmpu1(self, i, a): return "FCMP(%d, P1(%d), P1(%d));" % (i.crfd, i.ra, i.rb)
    def op_ps_cmpo1(self, i, a): return "FCMP(%d, P1(%d), P1(%d));" % (i.crfd, i.ra, i.rb)

    def _psq(self, i, load, ea, update):
        fn = "psq_load" if load else "psq_store"
        return "{ u32 ea_ = %s; %s(c, ea_, %d, %d, %d);%s }" % (ea, fn, i.rd, i.w_bit, i.qi, " R(%d) = ea_;" % i.ra if update else "")

    def op_psq_l(self, i, a): return self._psq(i, True, self.ea_d(i), False)
    def op_psq_lu(self, i, a): return self._psq(i, True, self.ea_d(i), True)
    def op_psq_st(self, i, a): return self._psq(i, False, self.ea_d(i), False)
    def op_psq_stu(self, i, a): return self._psq(i, False, self.ea_d(i), True)
    def op_psq_lx(self, i, a): return self._psq(i, True, self.ea_x(i), False)
    def op_psq_lux(self, i, a): return self._psq(i, True, self.ea_x(i), True)
    def op_psq_stx(self, i, a): return self._psq(i, False, self.ea_x(i), False)
    def op_psq_stux(self, i, a): return self._psq(i, False, self.ea_x(i), True)

    # --- condition register
    def _crop(self, i, expr):
        return "SETCRBIT(%d, %s);" % (i.rd, expr % ("CRBIT(%d)" % i.ra, "CRBIT(%d)" % i.rb))

    def op_crand(self, i, a): return self._crop(i, "(%s & %s)")
    def op_crandc(self, i, a): return self._crop(i, "(%s & !%s)")
    def op_cror(self, i, a): return self._crop(i, "(%s | %s)")
    def op_crorc(self, i, a): return self._crop(i, "(%s | !%s)")
    def op_crxor(self, i, a): return self._crop(i, "(%s ^ %s)")
    def op_crnor(self, i, a): return self._crop(i, "!(%s | %s)")
    def op_crnand(self, i, a): return self._crop(i, "!(%s & %s)")
    def op_creqv(self, i, a): return self._crop(i, "!(%s ^ %s)")
    def op_mcrf(self, i, a): return "c->cr[%d] = c->cr[%d];" % (i.crfd, i.crfs)
    def op_mfcr(self, i, a): return "R(%d) = ppc_get_cr(c);" % i.rd
    def op_mtcrf(self, i, a): return "ppc_set_cr(c, R(%d), 0x%02X);" % (i.rd, i.crm)
    def op_mcrxr(self, i, a): return "c->cr[%d] = (c->so << 3) | (c->ov << 2) | (c->ca << 1); c->so = c->ov = c->ca = 0;" % i.crfd

    # --- special registers
    def op_mfspr(self, i, a):
        s = i.spr
        if s == 8: return "R(%d) = c->lr;" % i.rd
        if s == 9: return "R(%d) = c->ctr;" % i.rd
        if s == 1: return "R(%d) = ppc_get_xer(c);" % i.rd
        if 912 <= s <= 919: return "R(%d) = c->gqr[%d];" % (i.rd, s - 912)
        return "R(%d) = rt_mfspr(c, %d);" % (i.rd, s)

    def op_mtspr(self, i, a):
        s = i.spr
        if s == 8: return "c->lr = R(%d);" % i.rd
        if s == 9: return "c->ctr = R(%d);" % i.rd
        if s == 1: return "ppc_set_xer(c, R(%d));" % i.rd
        if 912 <= s <= 919: return "c->gqr[%d] = R(%d);" % (s - 912, i.rd)
        return "rt_mtspr(c, %d, R(%d));" % (s, i.rd)

    def op_mftb(self, i, a): return "R(%d) = rt_mftb(c, %d);" % (i.rd, i.spr)
    def op_mfmsr(self, i, a): return "R(%d) = c->msr;" % i.rd
    def op_mtmsr(self, i, a): return "rt_mtmsr(c, R(%d));" % i.rd
    def op_mfsr(self, i, a): return "R(%d) = c->sr[%d];" % (i.rd, i.imm)
    def op_mtsr(self, i, a): return "c->sr[%d] = R(%d);" % (i.imm, i.rd)
    def op_mfsrin(self, i, a): return "R(%d) = c->sr[R(%d) >> 28];" % (i.rd, i.rb)
    def op_mtsrin(self, i, a): return "c->sr[R(%d) >> 28] = R(%d);" % (i.rb, i.rd)

    # --- cache / sync / system
    def op_dcbz(self, i, a): return "rt_dcbz(%s);" % self.ea_x(i)
    def op_dcbz_l(self, i, a): return "rt_dcbz(%s);" % self.ea_x(i)
    def _nop(self, i, a): return ";"
    op_dcbst = op_dcbf = op_dcbi = op_dcbt = op_dcbtst = op_icbi = op_sync = op_isync = op_eieio = _nop
    op_tlbie = op_tlbsync = _nop
    def op_sc(self, i, a): return "rt_sc(c);"
    def op_rfi(self, i, a): return "rt_rfi(c); return;"
    def op_tw(self, i, a): return "rt_trap(c, %s);" % hx(a) if i.rd == 31 else ";"
    def op_twi(self, i, a): return "rt_trap(c, %s);" % hx(a) if i.rd == 31 else ";"


# =========================================================================== patch variants
# runtime/patches/NAME.gecko holds Gecko codes (Dolphin GameSettings format).  Their unconditional writes into
# code (04 / 06) are compiled in: every function they change gets a variant generated from the patched code,
# fn_X dispatches on g_patch_on[n] (set by the runtime, host/patches.cpp) between fn_X__base and fn_X__NAME.
# Code caves are the written code entered by a written branch: one entered by `bl` becomes a function of its
# own, one entered by `b` (a detour that branches back) is compiled into the variants that branch to it.
# All the other codes (data writes, pointer codes, conditionals) are left to the runtime's Gecko interpreter,
# which runs them every VI field: build/recomp/patches.cpp.
PATCH_DIR = "runtime/patches"


def parse_gecko(path):
    codes = []
    for line in open(path):
        line = line.split(";")[0].strip()
        if not line or line.startswith("$") or line.startswith("*"):
            continue
        a, b = line.split()[:2]
        codes.append((int(a, 16), int(b, 16)))
    return codes


def split_gecko(prog, codes):
    """-> (code writes addr -> u32, the remaining codes for the runtime)."""
    writes, rest = {}, []
    depth = 0   # conditional nesting: only top-level writes can be compiled in
    i = 0
    while i < len(codes):
        a, v = codes[i]
        t = (a >> 24) & 0xFE
        n = 1
        if t == 0x06:
            n = 1 + (v + 7) // 8
        addr = 0x80000000 | (a & 0x01FFFFFF)
        if depth == 0 and t in (0x04, 0x06) and prog.in_text(addr):
            if t == 0x04:
                writes[addr] = v
            else:
                assert v % 4 == 0 and addr % 4 == 0, "06 code into code must be whole words"
                data = b"".join(struct.pack(">II", *codes[i + 1 + k]) for k in range(n - 1))[:v]
                for k in range(0, v, 4):
                    writes[addr + k] = struct.unpack_from(">I", data, k)[0]
        else:
            rest.extend(codes[i:i + n])
            if 0x20 <= t <= 0x2E or t == 0xDE:
                depth += 1
            elif t == 0xE2:
                depth = max(0, depth - (a & 0xFF))   # E2T000VV: VV endifs
            elif t == 0xE0:
                depth = 0
        i += n
    return writes, rest


def patched_program(prog, writes):
    data = bytearray(prog.dol.data)
    for a, w in writes.items():
        s = prog.dol.section_for(a)
        struct.pack_into(">I", data, s.offset + a - s.addr, w)
    p = Program(gcdol.Dol(bytes(data), prog.dol.sections, prog.dol.bss_addr, prog.dol.bss_size, prog.dol.entry))
    p.ptr_list, p.ptr_locs = prog.ptr_list, prog.ptr_locs
    return p


class PatchSet:
    def __init__(self, index, name, prog, funcs, codes):
        self.index, self.name = index, name
        self.writes, self.runtime_codes = split_gecko(prog, codes)
        self.prog = patched_program(prog, self.writes)
        written = frozenset(self.writes)
        # branch targets inside the written code; a target inside another target's cave is internal
        targets = {}
        for a in self.writes:
            i = self.prog.insn(a)
            if i is not None and i.op in ("b", "bc") and i.target in written:
                targets.setdefault(i.target, set()).add((a, bool(i.lk)))
        walks = {}
        for t in targets:
            seen = set()
            _reach_walk(self.prog, 0, 0, seen, [t], written)
            walks[t] = seen
        self.callable, self.detour = {}, set()   # entry -> (lo, hi); detour cave addresses
        cave_addrs = set()
        for t, srcs in targets.items():
            if any(t in walks[o] and o != t and not (o in walks[t]) for o in walks):
                continue   # internal branch of a cave
            body = walks[t]
            cave_addrs |= body
            from_outside = {lk for a, lk in srcs if a not in body}
            assert len(from_outside) == 1, "cave %08x entered both by b and bl" % t
            if True in from_outside:
                lo, hi = min(body), max(body) + 4
                assert body == set(range(lo, hi, 4)), "callable cave %08x is not contiguous" % t
                self.callable[t] = (lo, hi)
            else:
                self.detour |= body
        import bisect
        starts = [s for s, _ in funcs]
        self.funcs = sorted({funcs[bisect.bisect_right(starts, a) - 1] for a in self.writes if a not in cave_addrs})

    def suffix(self, s):
        return "fn_%08X__%s" % (s, self.name)


def load_patches(prog, funcs):
    out = []
    if not os.path.isdir(PATCH_DIR):
        return out
    for fn in sorted(os.listdir(PATCH_DIR)):
        if fn.endswith(".gecko"):
            out.append(PatchSet(len(out), fn[:-6], prog, funcs, parse_gecko(os.path.join(PATCH_DIR, fn))))
    return out


def write_patches(patches, funcs, names, hle, jump_targets, base_em):
    """patch_variants.cpp (variants, caves, dispatchers) and patches.cpp (runtime codes)."""
    lines = ["// generated by tools/recomp/recomp.py from %s/*.gecko" % PATCH_DIR, "#include \"funcs.h\"", ""]
    warn = []
    for p in patches:
        em = Emitter(p.prog, funcs, names, {}, jump_targets)
        em.alias = {t: p.suffix(t) for t in p.callable}
        for t, (lo, hi) in sorted(p.callable.items()):
            lines += em.body(lo, hi, "code cave (%s)" % p.name, p.suffix(t)) + [""]
        for s, e in p.funcs:
            assert s not in hle, "patched function %08x is HLE" % s
            lines += em.body(s, e, names.get(s), p.suffix(s), frozenset(p.detour)) + [""]
            lines += base_em.body(s, e, names.get(s), "fn_%08X__base" % s) + [""]
            lines.append("void fn_%08X(CPU* c) { if (g_patch_on[%d]) %s(c); else fn_%08X__base(c); }"
                         % (s, p.index, p.suffix(s), s))
            lines.append("")
        warn += em.warn
    open(os.path.join(OUT, "patch_variants.cpp"), "w").write("\n".join(lines) + "\n")
    with open(os.path.join(OUT, "patches.cpp"), "w") as f:
        f.write("// generated by tools/recomp/recomp.py: the Gecko codes of %s/*.gecko that the runtime runs\n"
                "// (host/patches.cpp); their writes into code are compiled in (patch_variants.cpp)\n" % PATCH_DIR)
        f.write("unsigned char g_patch_on[%d];\n" % max(1, len(patches)))
        for p in patches:
            f.write("static const unsigned k_%s[] = {\n" % p.name)
            for a, v in p.runtime_codes:
                f.write("  0x%08X, 0x%08X,\n" % (a, v))
            f.write("  0};\n")
        f.write("extern const unsigned g_patch_count = %d;\n" % len(patches))
        f.write("extern const char* const g_patch_names[] = {%s};\n"
                % ", ".join(["\"%s\"" % p.name for p in patches] or ["0"]))
        f.write("extern const unsigned* const g_patch_codes[] = {%s};\n"
                % ", ".join(["k_" + p.name for p in patches] or ["0"]))
        f.write("extern const unsigned g_patch_code_words[] = {%s};\n"
                % ", ".join([str(2 * len(p.runtime_codes)) for p in patches] or ["0"]))
    return warn


# =========================================================================== driver

def load_hle(path, names_rev):
    hle = {}
    if not os.path.exists(path):
        return hle
    for line in open(path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        key, sym = line.split()
        if key.startswith("0x"):
            addr = int(key, 16)
        elif key in names_rev:
            addr = names_rev[key]
        else:
            print("hle: unknown function", key)
            continue
        hle[addr] = sym
    return hle


def main():
    dol = gcdol.load_dol("orig/sys/main.dol")
    prog = Program(dol)
    seeds = set()
    if os.path.exists("build/rea_procedures.json"):
        seeds |= {int(p["address"], 16) for p in json.load(open("build/rea_procedures.json"))["normalized_result"]}
    names = {}
    if os.path.exists("build/matched_symbols.json"):
        for m in json.load(open("build/matched_symbols.json")):
            names[m["addr"]] = m["name"]
            seeds.add(m["addr"])
    seeds.add(dol.entry)
    ptrs = data_code_pointers(prog)
    prog.ptr_list = sorted(ptrs)
    prog.ptr_locs = ptrs
    # data pointers are either function pointers or jump-table entries; resolved after partitioning
    starts = find_functions(prog, seeds)
    funcs = ranges_from_starts(prog, starts)
    # function pointers in data that are not inside an existing function's reachable code
    fstart = set(starts)
    reached = {}
    for s, e in funcs:
        for a in reach(prog, s, e):
            reached[a] = s
    extra = set()
    for p in ptrs:
        if p not in fstart and p not in reached:
            extra.add(p)
    if extra:
        starts = find_functions(prog, set(starts) | extra)
        funcs = ranges_from_starts(prog, starts)
    # branches into the middle of another function: make the target an entry point
    # (the containing function then falls through into it with a tail call)
    while True:
        fstart = set(starts)
        extra = set()
        for s, e in funcs:
            for a in reach(prog, s, e):
                i = prog.insn(a)
                if i.op in ("b", "bc") and not i.lk and not (s <= i.target < e) \
                        and i.target not in fstart and prog.in_text(i.target):
                    extra.add(i.target)
        if not extra:
            break
        starts = sorted(fstart | extra)
        funcs = ranges_from_starts(prog, starts)
    jump_targets = sorted(p for p in ptrs if p not in set(starts))
    names_rev = {}
    for a, n in names.items():
        names_rev.setdefault(n, a)
    hle = load_hle("runtime/hle_functions.txt", names_rev)

    em = Emitter(prog, funcs, names, hle, jump_targets)
    patches = load_patches(prog, funcs)
    patched = {s for p in patches for s, _ in p.funcs}
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.startswith("funcs_") and f.endswith(".cpp"):
            os.remove(os.path.join(OUT, f))
    with open(os.path.join(OUT, "funcs.h"), "w") as h:
        h.write("// generated by tools/recomp/recomp.py\n#pragma once\n#include \"ppc_rt.h\"\n")
        for s, _ in funcs:
            h.write("void fn_%08X(CPU* c);\n" % s)
        for sym in sorted(set(hle.values())):
            h.write("void %s(CPU* c);\n" % sym)
        for a in sorted(hle):
            h.write("void orig_%08X(CPU* c);\n" % a)
        h.write("extern unsigned char g_patch_on[];   // patches.cpp; host/patches.cpp sets it\n")
        for p in patches:
            for t in sorted(p.callable):
                h.write("void %s(CPU* c);\n" % p.suffix(t))
            for s, _ in p.funcs:
                h.write("void %s(CPU* c);\nvoid fn_%08X__base(CPU* c);\n" % (p.suffix(s), s))
    nfile = 0
    for k in range(0, len(funcs), FUNCS_PER_FILE):
        with open(os.path.join(OUT, "funcs_%03d.cpp" % nfile), "w") as f:
            f.write("// generated by tools/recomp/recomp.py\n#include \"funcs.h\"\n\n")
            for s, e in funcs[k:k + FUNCS_PER_FILE]:
                if s in patched:
                    continue   # patch_variants.cpp
                f.write("\n".join(em.function(s, e)))
                f.write("\n\n")
        nfile += 1
    with open(os.path.join(OUT, "functable.cpp"), "w") as f:
        f.write("// generated by tools/recomp/recomp.py\n#include \"funcs.h\"\n\n")
        f.write("const FuncEntry g_func_table[] = {\n")
        for s, _ in funcs:
            f.write("  {0x%08X, fn_%08X},\n" % (s, s))
        f.write("};\nconst unsigned g_func_count = %d;\n" % len(funcs))
    json.dump([{"start": s, "end": e, "name": names.get(s)} for s, e in funcs],
              open(os.path.join(OUT, "functions.json"), "w"))
    em.warn += write_patches(patches, funcs, names, hle, jump_targets, em)
    for p in patches:
        print("patch %s: %d code words compiled in, functions %s, callable caves %s, %d detour cave words, "
              "%d runtime codes" % (p.name, len(p.writes), " ".join("%08X" % s for s, _ in p.funcs),
                                    " ".join("%08X" % t for t in sorted(p.callable)), len(p.detour),
                                    len(p.runtime_codes)))
    with open(os.path.join(OUT, "warnings.txt"), "w") as f:
        f.write("\n".join(em.warn))
    print("functions: %d  files: %d  jump-table targets: %d  hle: %d  warnings: %d"
          % (len(funcs), nfile, len(jump_targets), len(hle), len(em.warn)))
    kinds = {}
    for w in em.warn:
        k = re.sub(r"[0-9a-f]{8}", "X", w)
        kinds[k] = kinds.get(k, 0) + 1
    for k, v in sorted(kinds.items(), key=lambda x: -x[1])[:20]:
        print("  %5d %s" % (v, k))


if __name__ == "__main__":
    main()
