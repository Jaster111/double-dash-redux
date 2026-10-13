"""PowerPC 750CL (Gekko) instruction decoder.

decode(word) -> Insn with a mnemonic and the raw fields the code generator
needs.  Mnemonics are canonical (no simplified forms); record (Rc=1) forms are
reported through the `rc` field, not a trailing dot, and link/absolute bits of
branches through `lk`/`aa`.
"""


def _bits(w, a, b):
    """IBM bit numbering: bit 0 is the MSB.  Inclusive range a..b."""
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


class Insn:
    __slots__ = ("op", "w", "rd", "ra", "rb", "rc_reg", "imm", "rc", "lk", "aa", "bo", "bi", "target",
                 "sh", "mb", "me", "spr", "crm", "crfd", "crfs", "l", "w_bit", "qi", "fm", "oe")

    def __init__(self, op, w):
        self.op = op
        self.w = w
        self.rd = _bits(w, 6, 10)
        self.ra = _bits(w, 11, 15)
        self.rb = _bits(w, 16, 20)
        self.rc_reg = _bits(w, 21, 25)   # frC for A-form, MB for rlw*
        self.imm = 0
        self.rc = w & 1
        self.lk = w & 1
        self.aa = (w >> 1) & 1
        self.bo = self.rd
        self.bi = self.ra
        self.target = None
        self.sh = self.rb
        self.mb = _bits(w, 21, 25)
        self.me = _bits(w, 26, 30)
        self.spr = 0
        self.crm = 0
        self.crfd = self.rd >> 2
        self.crfs = self.ra >> 2
        self.l = (self.rd >> 0) & 1
        self.w_bit = 0
        self.qi = 0
        self.fm = 0
        self.oe = _bits(w, 21, 21)

    def __repr__(self):
        return "<%s %08x>" % (self.op, self.w)


_D_OPS = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.", 14: "addi", 15: "addis",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    3: "twi",
}
_UNSIGNED_IMM = {"cmpli", "ori", "oris", "xori", "xoris", "andi.", "andis."}

_OP19 = {0: "mcrf", 16: "bclr", 33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
         225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

_OP31 = {
    0: "cmp", 4: "tw", 19: "mfcr", 20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl",
    54: "dcbst", 55: "lwzux", 60: "andc", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 119: "lbzux", 124: "nor",
    144: "mtcrf", 146: "mtmsr", 150: "stwcx.", 151: "stwx", 183: "stwux", 210: "mtsr", 215: "stbx",
    242: "mtsrin", 246: "dcbtst", 247: "stbux", 278: "dcbt", 279: "lhzx", 284: "eqv", 306: "tlbie",
    310: "eciwx", 311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 371: "mftb", 375: "lhaux",
    407: "sthx", 412: "orc", 438: "ecowx", 439: "sthux", 444: "or", 467: "mtspr", 470: "dcbi", 476: "nand",
    512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw", 566: "tlbsync", 567: "lfsux",
    595: "mfsr", 597: "lswi", 598: "sync", 599: "lfdx", 631: "lfdux", 659: "mfsrin", 661: "stswx",
    662: "stwbrx", 663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx", 759: "stfdux", 790: "lhbrx",
    792: "sraw", 824: "srawi", 854: "eieio", 918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi",
    983: "stfiwx", 1014: "dcbz",
}
_OP31_XO = {8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
            138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw", 266: "add",
            459: "divwu", 491: "divw"}

_OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 28: "fmsubs",
         29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}
_OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
           28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
_OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
           64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs",
           711: "mtfsf"}

_OP4_A = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1",
          18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
          28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
_OP4_X6 = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}
_OP4_X10 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1",
            136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10",
            624: "ps_merge11", 1014: "dcbz_l"}


def decode(w, addr=0):
    opc = w >> 26
    if opc in _D_OPS:
        i = Insn(_D_OPS[opc], w)
        raw = w & 0xFFFF
        i.imm = raw if i.op in _UNSIGNED_IMM else _sext(raw, 16)
        i.rc = 0
        if i.op in ("cmpi", "cmpli"):
            i.l = (w >> 21) & 1
        return i
    if opc == 18:
        i = Insn("b", w)
        li = _sext(w & 0x03FFFFFC, 26)
        i.target = (li if i.aa else addr + li) & 0xFFFFFFFF
        return i
    if opc == 16:
        i = Insn("bc", w)
        bd = _sext(w & 0xFFFC, 16)
        i.target = (bd if i.aa else addr + bd) & 0xFFFFFFFF
        return i
    if opc == 17:
        return Insn("sc", w)
    if opc in (20, 21, 23):
        i = Insn({20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[opc], w)
        return i
    if opc == 19:
        xo = _bits(w, 21, 30)
        if xo in _OP19:
            i = Insn(_OP19[xo], w)
            if i.op == "mcrf":
                i.crfd = i.rd >> 2
                i.crfs = i.ra >> 2
            return i
        return None
    if opc == 31:
        xo = _bits(w, 21, 30)
        if xo in _OP31:
            i = Insn(_OP31[xo], w)
            if i.op in ("mfspr", "mtspr", "mftb"):
                s = _bits(w, 11, 20)
                i.spr = ((s & 0x1F) << 5) | (s >> 5)
            elif i.op == "mtcrf":
                i.crm = _bits(w, 12, 19)
            elif i.op in ("cmp", "cmpl"):
                i.l = (w >> 21) & 1
            elif i.op in ("mtsr", "mfsr"):
                i.imm = _bits(w, 12, 15)
            return i
        xo9 = _bits(w, 22, 30)
        if xo9 in _OP31_XO:
            i = Insn(_OP31_XO[xo9], w)
            i.oe = _bits(w, 21, 21)
            return i
        return None
    if opc == 59:
        xo = _bits(w, 26, 30)
        if xo in _OP59:
            return Insn(_OP59[xo], w)
        return None
    if opc == 63:
        xo5 = _bits(w, 26, 30)
        if xo5 in _OP63_A and xo5 >= 18:
            return Insn(_OP63_A[xo5], w)
        xo = _bits(w, 21, 30)
        if xo in _OP63_X:
            i = Insn(_OP63_X[xo], w)
            if i.op == "mtfsf":
                i.fm = _bits(w, 7, 14)
            return i
        return None
    if opc in (56, 57, 60, 61):
        i = Insn({56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}[opc], w)
        i.w_bit = _bits(w, 16, 16)
        i.qi = _bits(w, 17, 19)
        i.imm = _sext(w & 0xFFF, 12)
        i.rc = 0
        return i
    if opc == 4:
        xo10 = _bits(w, 21, 30)
        if xo10 in _OP4_X10:
            return Insn(_OP4_X10[xo10], w)
        xo6 = _bits(w, 25, 30)
        if xo6 in _OP4_X6:
            i = Insn(_OP4_X6[xo6], w)
            i.w_bit = _bits(w, 21, 21)
            i.qi = _bits(w, 22, 24)
            i.rc = 0
            return i
        xo5 = _bits(w, 26, 30)
        if xo5 in _OP4_A:
            return Insn(_OP4_A[xo5], w)
        return None
    return None
