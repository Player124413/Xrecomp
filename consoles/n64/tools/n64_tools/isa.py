"""MIPS III (VR4300) decoder (XBA-170): every instruction the N64 CPU runs, with what the lifter and the discovery need
to know (class, registers read and written, branch targets, links). Our own table, pure Python; decoding follows the
interpreter (consoles/n64/runtime/n64_cpu.c, n64_fpu.c): only the fields it looks at select an instruction.

Classes:
  alu      plain GPR / HI / LO write, no exception (shifts, logic, ADDU-type, SLT, LUI, MFHI / MFLO, 64-bit forms)
  ovf      ADD ADDI SUB DADD DADDI DSUB: an overflow raises
  trap     TGE TGEU TLT TLTU TEQ TNE and the immediate forms: raise when the condition holds
  muldiv   MULT MULTU DIV DIVU DMULT DMULTU DDIV DDIVU MTHI MTLO
  load     LB LH LW LBU LHU LWU LD, partial LWL LWR LDL LDR
  store    SB SH SW SD, partial SWL SWR SDL SDR
  branch   conditional PC-relative branch (BEQ BNE BLEZ BGTZ, REGIMM, BC1F / BC1T), likely forms, optional link
  jump     J / JAL (static target)
  jr       JR / JALR (target in a register)
  fmove    MFC1 DMFC1 CFC1 MTC1 DMTC1 CTC1
  fmem     LWC1 LDC1 SWC1 SDC1
  fop      the other COP1 instructions (arithmetic, conversions, compares, reserved formats): run by n64_fpu.c
  nop      CACHE and SYNC (no cache model: they do nothing in kernel mode)
  interp   left to the interpreter: SYSCALL, BREAK, COP0 (MFC0 MTC0 TLB* ERET ...), COP2 / COP3 and their loads /
           stores, LL LLD SC SCD, reserved encodings
"""

R0, AT, V0, V1, A0, A1, A2, A3 = range(8)
T0, T1, T2, T3 = 8, 9, 10, 11
T9, K0, K1, GP, SP, FP, RA = 25, 26, 27, 28, 29, 30, 31

REG_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr",
    0x0C: "syscall", 0x0D: "break", 0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x14: "dsllv", 0x16: "dsrlv", 0x17: "dsrav", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x1C: "dmult", 0x1D: "dmultu", 0x1E: "ddiv", 0x1F: "ddivu", 0x20: "add", 0x21: "addu", 0x22: "sub",
    0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd",
    0x2D: "daddu", 0x2E: "dsub", 0x2F: "dsubu", 0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu", 0x34: "teq",
    0x36: "tne", 0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra", 0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32",
}
REGIMM = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x08: "tgei", 0x09: "tgeiu", 0x0A: "tlti",
    0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei", 0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
}
PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi", 0x09: "addiu",
    0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x14: "beql", 0x15: "bnel",
    0x16: "blezl", 0x17: "bgtzl", 0x18: "daddi", 0x19: "daddiu", 0x1A: "ldl", 0x1B: "ldr", 0x20: "lb", 0x21: "lh",
    0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x27: "lwu", 0x28: "sb", 0x29: "sh",
    0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr", 0x2E: "swr", 0x2F: "cache", 0x30: "ll", 0x31: "lwc1",
    0x32: "lwc2", 0x34: "lld", 0x35: "ldc1", 0x36: "ldc2", 0x37: "ld", 0x38: "sc", 0x39: "swc1", 0x3A: "swc2",
    0x3C: "scd", 0x3D: "sdc1", 0x3E: "sdc2", 0x3F: "sd",
}
COP1_MOVES = {0x00: "mfc1", 0x01: "dmfc1", 0x02: "cfc1", 0x04: "mtc1", 0x05: "dmtc1", 0x06: "ctc1"}

LOADS = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "lwu": 4, "ld": 8, "lwl": 4, "lwr": 4, "ldl": 8, "ldr": 8}
STORES = {"sb": 1, "sh": 2, "sw": 4, "sd": 8, "swl": 4, "swr": 4, "sdl": 8, "sdr": 8}
OVF = {"add", "addi", "sub", "dadd", "daddi", "dsub"}
TRAPS = {"tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"}
MULDIV = {"mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu", "mthi", "mtlo"}
INTERP = {"syscall", "break", "cop0", "cop2", "cop3", "lwc2", "ldc2", "swc2", "sdc2", "ll", "lld", "sc", "scd",
          "reserved"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall", "bc1fl", "bc1tl"}


def sx32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


class Ins:
    __slots__ = ("addr", "word", "name", "cls", "rs", "rt", "rd", "sa", "imm", "simm", "target", "link", "likely",
                 "fs", "ft", "fd")

    def __init__(self, addr, word):
        self.addr = addr & 0xFFFFFFFF
        self.word = word
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.fs, self.ft, self.fd = self.rd, self.rt, self.sa
        self.target = None  # static branch / jump target (32-bit)
        self.link = None    # register written with the return address
        self.likely = False
        _decode(self)

    # ---- register usage (the interpreter's semantics) ----
    def reads(self):
        """GPRs read (r0 excluded)."""
        n, c = self.name, self.cls
        out = ()
        if c == "alu":
            op = self.word >> 26
            if op == 0:
                if n in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
                    out = (self.rt,)
                elif n in ("mfhi", "mflo", "sync"):
                    out = ()
                else:
                    out = (self.rs, self.rt)
            elif n == "lui":
                out = ()
            else:
                out = (self.rs,)
        elif c == "ovf":
            out = (self.rs,) if n in ("addi", "daddi") else (self.rs, self.rt)
        elif c == "trap":
            out = (self.rs,) if n.endswith("i") or n.endswith("iu") else (self.rs, self.rt)
        elif c == "muldiv":
            out = (self.rs,) if n in ("mthi", "mtlo") else (self.rs, self.rt)
        elif c == "load":
            out = (self.rs, self.rt) if n in ("lwl", "lwr", "ldl", "ldr") else (self.rs,)
        elif c == "store":
            out = (self.rs, self.rt)
        elif c == "branch":
            if n in ("beq", "bne", "beql", "bnel"):
                out = (self.rs, self.rt)
            elif n.startswith("bc1"):
                out = ()
            else:
                out = (self.rs,)
        elif c == "jr":
            out = (self.rs,)
        elif c == "fmove":
            out = (self.rt,) if n in ("mtc1", "dmtc1", "ctc1") else ()
        elif c in ("fmem", "cache"):
            out = (self.rs,)
        return tuple(r for r in out if r)

    def writes(self):
        """GPRs written (r0 excluded)."""
        n, c = self.name, self.cls
        d = None
        if c == "alu":
            if n == "sync":
                d = None
            elif (self.word >> 26) == 0:
                d = self.rd
            else:
                d = self.rt
        elif c == "ovf":
            d = self.rt if n in ("addi", "daddi") else self.rd
        elif c == "load":
            d = self.rt
        elif c == "fmove" and n in ("mfc1", "dmfc1", "cfc1"):
            d = self.rt
        out = [d] if d else []
        if self.link:
            out.append(self.link)
        return tuple(out)

    def dest(self):
        w = self.writes()
        return w[0] if w and not self.link else None

    def is_transfer(self):
        return self.cls in ("branch", "jump", "jr")

    def is_unconditional(self):
        """J, JR, B (BEQ r0, r0 / BEQL r0, r0 / BGEZ r0): control never falls through past the delay slot."""
        if self.cls in ("jump", "jr"):
            return True
        if self.cls == "branch":
            if self.name in ("beq", "beql") and self.rs == 0 and self.rt == 0:
                return True
            if self.name in ("bgez", "bgezl", "bgezal", "bgezall") and self.rs == 0:
                return True
        return False

    def is_call(self):
        return self.name in ("jal", "jalr", "bgezal", "bltzal", "bgezall", "bltzall")

    def text(self):
        n = self.name
        r = REG_NAMES
        c = self.cls
        if n == "reserved":
            return ".word 0x%08X" % self.word
        if c == "jump" or (c == "branch" and n.startswith("bc1")):
            return "%s 0x%08X" % (n, self.target)
        if c == "branch":
            if n in ("beq", "bne", "beql", "bnel"):
                return "%s %s, %s, 0x%08X" % (n, r[self.rs], r[self.rt], self.target)
            return "%s %s, 0x%08X" % (n, r[self.rs], self.target)
        if c == "jr":
            return "%s %s" % (n, r[self.rs]) if n == "jr" else "jalr %s, %s" % (r[self.rd], r[self.rs])
        if c in ("load", "store"):
            return "%s %s, %d(%s)" % (n, r[self.rt], self.simm, r[self.rs])
        if c == "fmem":
            return "%s f%d, %d(%s)" % (n, self.rt, self.simm, r[self.rs])
        if c == "fmove":
            return "%s %s, f%d" % (n, r[self.rt], self.fs)
        if c == "fop":
            return "cop1 0x%07X" % (self.word & 0x1FFFFFF)
        if n == "lui":
            return "lui %s, 0x%04X" % (r[self.rt], self.imm)
        if (self.word >> 26) == 0 and c in ("alu", "ovf", "muldiv", "trap"):
            if n in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
                if self.word == 0:
                    return "nop"
                return "%s %s, %s, %d" % (n, r[self.rd], r[self.rt], self.sa)
            if n in ("mfhi", "mflo"):
                return "%s %s" % (n, r[self.rd])
            if n in ("mthi", "mtlo"):
                return "%s %s" % (n, r[self.rs])
            if c in ("muldiv", "trap"):
                return "%s %s, %s" % (n, r[self.rs], r[self.rt])
            return "%s %s, %s, %s" % (n, r[self.rd], r[self.rs], r[self.rt])
        if c in ("alu", "ovf", "trap") and (self.word >> 26) != 0:
            if c == "trap":
                return "%s %s, %d" % (n, r[self.rs], self.simm)
            return "%s %s, %s, %d" % (n, r[self.rt], r[self.rs],
                                       self.imm if n in ("andi", "ori", "xori") else self.simm)
        return n


def _decode(i):
    w = i.word
    op = w >> 26
    name = None
    if op == 0x00:
        name = SPECIAL.get(w & 63)
    elif op == 0x01:
        name = REGIMM.get(i.rt)
    elif op == 0x10:
        name = "cop0"
    elif op == 0x11:
        sub = i.rs
        if sub in COP1_MOVES:
            name = COP1_MOVES[sub]
        elif sub == 0x08:
            name = ("bc1f", "bc1t", "bc1fl", "bc1tl")[i.rt & 3]
        else:
            name = "fop"  # arithmetic, conversions, compares; reserved formats raise in n64_fpu.c
    elif op == 0x12:
        name = "cop2"
    elif op == 0x13:
        name = "cop3"
    else:
        name = PRIMARY.get(op)
    if name is None:
        name = "reserved"
    i.name = name
    if name in INTERP:
        i.cls = "interp"
    elif name == "cache":
        i.cls = "cache"  # the core keeps the caches' tags (n64_rc_cache)
    elif name == "sync":
        i.cls = "nop"
    elif name in LOADS:
        i.cls = "load"
    elif name in STORES:
        i.cls = "store"
    elif name in OVF:
        i.cls = "ovf"
    elif name in TRAPS:
        i.cls = "trap"
    elif name in MULDIV:
        i.cls = "muldiv"
    elif name in ("j", "jal"):
        i.cls = "jump"
        i.target = (((i.addr + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) & 0xFFFFFFFF
        if name == "jal":
            i.link = RA
    elif name in ("jr", "jalr"):
        i.cls = "jr"
        if name == "jalr" and i.rd:
            i.link = i.rd
    elif name in COP1_MOVES.values():
        i.cls = "fmove"
    elif name in ("lwc1", "ldc1", "swc1", "sdc1"):
        i.cls = "fmem"
    elif name == "fop":
        i.cls = "fop"
    elif name.startswith("b"):
        i.cls = "branch"
        i.target = (i.addr + 4 + (i.simm << 2)) & 0xFFFFFFFF
        i.likely = name in LIKELY
        if name in ("bltzal", "bgezal", "bltzall", "bgezall"):
            i.link = RA
        # a target across the 2 GB boundary is not a sign-extended 32-bit address: the interpreter handles it
        if (i.addr ^ i.target) & 0x80000000:
            i.cls = "interp"
            i.target = None
            i.link = None
    else:
        i.cls = "alu"


def decode(word, addr=0):
    return Ins(addr, word)
