"""VR4300 (MIPS III) to C (XBA-170): one C function per discovered function, in the shape of n64_recomp_rt.h.

The function is cut into blocks, each entered from the entry switch at its first instruction (a resume point), with
one event check per segment (« blocks and event checks » below): lifted code stops where the interpreter would
(scheduler event, interrupt) and the interpreter runs the rest of that block. Delay slots run inline after their
branch (and are emitted on their own too only when a branch of the function targets them). Semantics are the
interpreter's (n64_cpu.c / n64_fpu.c), instruction for instruction:
- 64-bit registers in locals (only those the function uses are loaded, only those it writes stored back), 32-bit
  results sign-extended, SRA / SRAV shifting the whole register, the DIV / DDIV results for a zero divisor;
- the fetch cost and the count of every instruction (RC_I), multiply / divide latencies, at the same point;
- branch delay slots inline; branch-likely skips its slot when not taken; a branch in a delay slot, COP0 (but
  CACHE), SYSCALL / BREAK, LL / SC, COP2 / COP3 and reserved encodings go to the interpreter, and so does any
  instruction whose exception condition holds (overflow, a trap taken, COP1 unusable): RC_INTERP, nothing of it done,
  so the interpreter raises with the exact EPC / BD;
- memory: cached RDRAM inline, anything else handed to the interpreter (« memory accesses » below), accesses to
  known IO registers through the core;
- COP1 moves, loads and stores inline (the FR register views), arithmetic and CTC1 through n64_fpu.c;
- calls, returns, indirect jumps and jumps out of the function return to the dispatcher (bounded C stack).
"""

from .discover import is_direct
from .isa import sx32


def hx(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def c64(v):
    return "0x%016XULL" % (v & 0xFFFFFFFFFFFFFFFF)


def sx(v):
    """The sign-extended 64-bit form of a 32-bit address, as a C constant."""
    return c64(sx32(v))


def R(r):
    return "0ull" if r == 0 else "r%d" % r


LOAD_CONV = {"lb": "RC_S8", "lbu": "RC_U8", "lh": "RC_S16", "lhu": "RC_U16", "lw": "RC_S32", "lwu": "RC_U32",
             "ld": "RC_U64"}
COST = {"mult": "N64_RC_COST_MULT", "multu": "N64_RC_COST_MULT", "div": "N64_RC_COST_DIV",
        "divu": "N64_RC_COST_DIV", "dmult": "N64_RC_COST_DMULT", "dmultu": "N64_RC_COST_DMULT",
        "ddiv": "N64_RC_COST_DDIV", "ddivu": "N64_RC_COST_DDIV"}
MULFN = {"mult": "rc_mult", "multu": "rc_multu", "div": "rc_div", "divu": "rc_divu", "dmult": "rc_dmult",
         "dmultu": "rc_mulu64", "ddiv": "rc_ddiv", "ddivu": "rc_ddivu"}


class FnLifter:
    def __init__(self, func, name, resume_all=False):
        self.resume_all = resume_all  # every instruction a block of its own (debugging)
        self.f = func
        self.img = func.image
        self.name = name
        self.fallbacks = 0
        self.labels = 0
        self.out = []
        self.targets = set()  # branch / jump targets inside the function
        for a in func.addrs:
            t = self.img.ins(a).target
            if t is not None and func.start <= t < func.end:
                self.targets.add(t)
        self.io = io_accesses(func)  # accesses to known constant addresses off cached RDRAM: through the core
        self.seg = None  # (emitted, units, leaders) while function() emits

    def inside(self, a):
        return self.f.start <= a < self.f.end

    def emit(self, s):
        self.out.append(s)

    def go(self, t):
        """Continue at static address t."""
        return "goto L_%08X;" % t if self.inside(t) else "RC_EXIT(%s);" % hx(t)

    # ---- one instruction (not a transfer) ----
    def body(self, i, fault, ds, cont="", hand=None):
        """C for instruction i at its address; fault: the statement that hands it to the interpreter; ds: 1 in a
        delay slot; cont: the re-check of the rest of its segment after a call into the core; hand: the hand-off of
        an access off cached RDRAM with the rest of its block (« memory accesses »; None in a delay slot)."""
        n, c = i.name, i.cls
        if c in ("load", "store", "fmem") and hand and i.addr not in self.io:
            return self.ram_access(i, fault, hand)
        rs, rt, rd = R(i.rs), R(i.rt), R(i.rd)
        simm = i.simm

        def w(reg, expr):
            return "" if reg == 0 else "%s = %s;" % (R(reg), expr)

        def dst(reg):
            return "dz" if reg == 0 else R(reg)

        ea = "%s + %s" % (rs, c64(simm)) if simm else rs
        if c == "interp":
            return fault
        if c == "nop":
            return ""
        if c == "cache":
            return "RC_CACHE(%d, %s, %s);" % (i.rt, ea, fault)
        if c == "alu":
            if (i.word >> 26) == 0:
                sa = i.sa
                ex = {
                    "sll": "RC_SX((uint32_t)%s << %d)" % (rt, sa), "srl": "RC_SX((uint32_t)%s >> %d)" % (rt, sa),
                    "sra": "RC_SX((uint64_t)((int64_t)%s >> %d))" % (rt, sa),
                    "sllv": "RC_SX((uint32_t)%s << (%s & 31))" % (rt, rs),
                    "srlv": "RC_SX((uint32_t)%s >> (%s & 31))" % (rt, rs),
                    "srav": "RC_SX((uint64_t)((int64_t)%s >> (%s & 31)))" % (rt, rs),
                    "dsllv": "%s << (%s & 63)" % (rt, rs), "dsrlv": "%s >> (%s & 63)" % (rt, rs),
                    "dsrav": "(uint64_t)((int64_t)%s >> (%s & 63))" % (rt, rs),
                    "mfhi": "hi", "mflo": "lo",
                    "addu": "RC_SX((uint32_t)%s + (uint32_t)%s)" % (rs, rt),
                    "subu": "RC_SX((uint32_t)%s - (uint32_t)%s)" % (rs, rt),
                    "and": "%s & %s" % (rs, rt), "or": "%s | %s" % (rs, rt), "xor": "%s ^ %s" % (rs, rt),
                    "nor": "~(%s | %s)" % (rs, rt),
                    "slt": "(uint64_t)((int64_t)%s < (int64_t)%s)" % (rs, rt),
                    "sltu": "(uint64_t)(%s < %s)" % (rs, rt),
                    "daddu": "%s + %s" % (rs, rt), "dsubu": "%s - %s" % (rs, rt),
                    "dsll": "%s << %d" % (rt, sa), "dsrl": "%s >> %d" % (rt, sa),
                    "dsra": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa),
                    "dsll32": "%s << %d" % (rt, sa + 32), "dsrl32": "%s >> %d" % (rt, sa + 32),
                    "dsra32": "(uint64_t)((int64_t)%s >> %d)" % (rt, sa + 32),
                }.get(n)
                if n == "sync":
                    return ""
                return w(i.rd, ex)
            ex = {
                "addiu": "RC_SX((uint32_t)%s + %s)" % (rs, hx(simm)),
                "daddiu": "%s + %s" % (rs, c64(simm)),
                "slti": "(uint64_t)((int64_t)%s < %dLL)" % (rs, simm),
                "sltiu": "(uint64_t)(%s < %s)" % (rs, c64(simm)),
                "andi": "%s & 0x%Xull" % (rs, i.imm), "ori": "%s | 0x%Xull" % (rs, i.imm),
                "xori": "%s ^ 0x%Xull" % (rs, i.imm), "lui": c64(sx32(i.imm << 16)),
            }[n]
            return w(i.rt, ex)
        if c == "ovf":
            mac = {"add": "RC_ADD32", "addi": "RC_ADD32", "sub": "RC_SUB32", "dadd": "RC_ADD64",
                   "daddi": "RC_ADD64", "dsub": "RC_SUB64"}[n]
            if n in ("addi", "daddi"):
                return "%s(%s, %s, %dLL, %s);" % (mac, dst(i.rt), rs, simm, fault)
            return "%s(%s, %s, %s, %s);" % (mac, dst(i.rd), rs, rt, fault)
        if c == "trap":
            cond = {
                "tge": "(int64_t)%s >= (int64_t)%s" % (rs, rt), "tgeu": "%s >= %s" % (rs, rt),
                "tlt": "(int64_t)%s < (int64_t)%s" % (rs, rt), "tltu": "%s < %s" % (rs, rt),
                "teq": "%s == %s" % (rs, rt), "tne": "%s != %s" % (rs, rt),
                "tgei": "(int64_t)%s >= %dLL" % (rs, simm), "tgeiu": "%s >= %s" % (rs, c64(simm)),
                "tlti": "(int64_t)%s < %dLL" % (rs, simm), "tltiu": "%s < %s" % (rs, c64(simm)),
                "teqi": "%s == %s" % (rs, c64(simm)), "tnei": "%s != %s" % (rs, c64(simm)),
            }[n]
            return "if (%s) { %s }" % (cond, fault)
        if c == "muldiv":
            if n == "mthi":
                return "hi = %s;" % rs
            if n == "mtlo":
                return "lo = %s;" % rs
            return "%s(%s, %s, &hi, &lo); now += %s;" % (MULFN[n], rs, rt, COST[n])
        if c == "load":
            if n in LOAD_CONV:
                return "RC_LOAD(%s, %s, %d, %s, %s, %s);" % (dst(i.rt), ea, SIZES[n], LOAD_CONV[n], fault, cont)
            sz = 4 if n in ("lwl", "lwr") else 8
            set_ = "%s = rc_%s(%s, w_, ea_);" % (R(i.rt), n, rt) if i.rt else ""
            return "{ uint64_t ea_ = %s, w_ = 0; RC_LOADP(w_, ea_, %d, %s, %s); %s }" % (ea, sz, fault, cont, set_)
        if c == "store":
            if n in ("sb", "sh", "sw", "sd"):
                return "RC_STORE(%s, %d, %s, %s, %s);" % (ea, SIZES[n], rt, fault, cont)
            sz = 4 if n in ("swl", "swr") else 8
            m = n.upper()
            return ("{ uint64_t ea_ = %s; RC_STOREP(ea_, RC_%s_D(%s, ea_), RC_%s_M(ea_), %d, %s, %s); }"
                    % (ea, m, rt, m, sz, fault, cont))
        cu = "if (!RC_CU1()) { %s } " % fault
        if c == "fmove":
            fs = i.fs
            if n == "mfc1":
                return cu + w(i.rt, "RC_S32(rc_fpu_read_s(h, sr, %d))" % fs)
            if n == "dmfc1":
                return cu + w(i.rt, "rc_fpu_read_d(h, sr, %d)" % fs)
            if n == "cfc1":
                return cu + w(i.rt, "rc_cfc1(h, %d)" % fs)
            if n == "mtc1":
                return cu + "rc_fpu_write_s(h, sr, %d, (uint32_t)%s);" % (fs, rt)
            if n == "dmtc1":
                return cu + "rc_fpu_write_d(h, sr, %d, %s);" % (fs, rt)
            if n == "ctc1":
                return cu + ("RC_CTC1(%s, %s, %d, %s);" % (rt, hx(i.addr), ds, cont) if fs == 31 else "")
        if c == "fmem":
            ft = i.ft
            if n == "lwc1":
                return cu + ("{ uint64_t fv_ = 0; RC_LOAD(fv_, %s, 4, RC_U64, %s, %s); rc_fpu_write_s(h, sr, %d, "
                             "(uint32_t)fv_); }" % (ea, fault, cont, ft))
            if n == "ldc1":
                return cu + ("{ uint64_t fv_ = 0; RC_LOAD(fv_, %s, 8, RC_U64, %s, %s); rc_fpu_write_d(h, sr, %d, "
                             "fv_); }" % (ea, fault, cont, ft))
            if n == "swc1":
                return cu + "RC_STORE(%s, 4, rc_fpu_read_s(h, sr, %d), %s, %s);" % (ea, ft, fault, cont)
            if n == "sdc1":
                return cu + "RC_STORE(%s, 8, rc_fpu_read_d(h, sr, %d), %s, %s);" % (ea, ft, fault, cont)
        if c == "fop":
            return cu + "RC_COP1(0x%08Xu, %s, %d, %s);" % (i.word, hx(i.addr), ds, cont)
        raise ValueError("cannot lift %s" % i.text())

    def ram_access(self, i, fault, hand):
        """A load or store whose address the lifter does not know: cached RDRAM inline, anything else handed to the
        interpreter with the rest of its block, nothing done."""
        n, rs, rt = i.name, R(i.rs), R(i.rt)
        ea = "%s + %s" % (rs, c64(i.simm)) if i.simm else rs
        dst = "dz" if i.rt == 0 else rt
        if n in LOAD_CONV:
            return "RC_LOAD_RAM(%s, %s, %d, %s, %s);" % (dst, ea, SIZES[n], LOAD_CONV[n], hand)
        if n in ("lwl", "lwr", "ldl", "ldr"):
            set_ = "%s = rc_%s(%s, w_, ea_);" % (rt, n, rt) if i.rt else ""
            return "{ uint64_t ea_ = %s, w_ = 0; RC_LOADP_RAM(w_, ea_, %d, %s); %s }" % (ea, SIZES[n], hand, set_)
        if n in ("sb", "sh", "sw", "sd"):
            return "RC_STORE_RAM(%s, %d, %s, %s);" % (ea, SIZES[n], rt, hand)
        if n in ("swl", "swr", "sdl", "sdr"):
            m = n.upper()
            return ("{ uint64_t ea_ = %s; RC_STOREP_RAM(ea_, RC_%s_D(%s, ea_), RC_%s_M(ea_), %d, %s); }"
                    % (ea, m, rt, m, SIZES[n], hand))
        cu = "if (!RC_CU1()) { %s } " % fault
        ft = i.ft
        if n == "lwc1":
            return cu + ("{ uint64_t fv_ = 0; RC_LOAD_RAM(fv_, %s, 4, RC_U64, %s); rc_fpu_write_s(h, sr, %d, "
                         "(uint32_t)fv_); }" % (ea, hand, ft))
        if n == "ldc1":
            return cu + ("{ uint64_t fv_ = 0; RC_LOAD_RAM(fv_, %s, 8, RC_U64, %s); rc_fpu_write_d(h, sr, %d, fv_); }"
                         % (ea, hand, ft))
        if n == "swc1":
            return cu + "RC_STORE_RAM(%s, 4, rc_fpu_read_s(h, sr, %d), %s);" % (ea, ft, hand)
        return cu + "RC_STORE_RAM(%s, 8, rc_fpu_read_d(h, sr, %d), %s);" % (ea, ft, hand)

    # ---- instructions ----
    def plain(self, a, left=1, cont=""):
        """Instruction a (not a transfer), left instructions before the end of its block; cont: the re-check of the
        rest of its block after a call into the core."""
        i = self.img.ins(a)
        code = self.body(i, "RC_INTERP(%s);" % hx(a), 0, cont, "RC_HAND(%s, %d);" % (hx(a), left))
        if i.cls == "interp":
            self.fallbacks += 1
        if code:
            self.emit("    %s\n" % code)

    def transfer(self, a):
        """A branch / jump at a with its delay slot inline (every path ends in a goto or an exit). The slot's event
        check is its segment's."""
        i = self.img.ins(a)
        s = a + 4
        if not self.inside(s) or self.img.ins(s).is_transfer():
            self.emit("    RC_INTERP(%s); /* no delay slot here / branch in a delay slot */\n" % hx(a))
            self.fallbacks += 1
            return
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        static = i.target is not None
        if i.cls == "branch":
            if n.startswith("bc1"):
                self.emit("    if (!RC_CU1()) RC_INTERP(%s);\n" % hx(a))
                cond = "(h->cpu.fcsr & RC_FCSR_C) %s 0" % ("!=" if n in ("bc1t", "bc1tl") else "==")
            elif n in ("beq", "beql"):
                cond = "%s == %s" % (rs, rt)
            elif n in ("bne", "bnel"):
                cond = "%s != %s" % (rs, rt)
            elif n in ("blez", "blezl"):
                cond = "(int64_t)%s <= 0" % rs
            elif n in ("bgtz", "bgtzl"):
                cond = "(int64_t)%s > 0" % rs
            elif n.startswith("bltz"):
                cond = "(int64_t)%s < 0" % rs
            else:
                cond = "(int64_t)%s >= 0" % rs
            self.emit("    tk = %s;\n" % cond)
        elif i.cls == "jr":
            self.emit("    jt = %s;\n" % rs)
        if i.link:
            self.emit("    %s = %s;\n" % (R(i.link), c64(sx32(a) + 8)))
        tgt = sx(i.target) if static else "jt"
        fall = c64(sx32(a) + 8)
        if i.cls == "branch" and i.likely:
            self.emit("    if (!tk) { %s }\n" % self.go(a + 8))
            npc = tgt
        elif i.cls == "branch":
            npc = "tk ? %s : %s" % (tgt, fall)
        else:
            npc = tgt
        si = self.img.ins(s)
        self.emit("    RC_I();\n")
        code = self.body(si, "RC_INTERP_SLOT(%s, %s);" % (hx(s), npc), 1)
        if si.cls == "interp":
            self.fallbacks += 1
        if code:
            self.emit("    %s\n" % code)
        idle = self.idle_loop(a)
        if i.cls == "branch" and not i.likely:
            self.emit("    if (tk) { %s%s }\n" % (idle, self.go(i.target)))
            self.emit("    %s\n" % self.go(a + 8))
        elif static:
            self.emit("    %s%s\n" % (idle, self.go(i.target)))
        else:
            self.emit("    RC_EXIT64(jt);\n")

    def idle_loop(self, a):
        """For a branch to itself over a NOP (an idle loop: nothing changes while it spins), the RC_IDLE that runs at
        once the turns its segment check would let run, else ""."""
        i = self.img.ins(a)
        if (i.cls != "branch" or i.link or i.name.startswith("bc1") or i.target != a or self.img.word(a + 4) != 0
                or self.seg is None):
            return ""
        emitted, units, leaders = self.seg
        if a not in leaders:
            return ""
        return "RC_IDLE(%d); " % segment_ticks(emitted, emitted.index(a), units, leaders)

    # ---- one function ----
    def function(self, comments=True):
        """The C function: blocks of instructions, each entered at its first instruction (a resume point), with one
        event check per segment."""
        f, img = self.f, self.img
        cpi = 20 if 0xA0000000 <= f.start < 0xC0000000 else 3
        cpi_name = "N64_RC_CPI_UNCACHED" if cpi == 20 else "N64_RC_CPI_CACHED"
        # which instructions are emitted: all but delay slots (they run inline after their branch) unless targeted
        emitted, slot = [], set()
        for a in f.addrs:
            i = img.ins(a)
            if a not in slot or a in self.targets:
                emitted.append(a)
            if i.is_transfer() and self.inside(a + 4) and not img.ins(a + 4).is_transfer():
                slot.add(a + 4)
        # what each runs in sequence, itself and its inline slot: [(ticks at most, dynamic)] (nothing is dynamic now:
        # calls into the core re-check the block after them)
        units = {}
        for a in emitted:
            i = img.ins(a)
            units[a] = [self.unit(i, cpi)]
            if i.is_transfer() and self.inside(a + 4) and not img.ins(a + 4).is_transfer():
                units[a].append(self.unit(img.ins(a + 4), cpi))
        leaders = self.block_leaders(emitted, units)
        self.labels = len(leaders)
        ends, end = {}, f.end  # instruction -> the address after its block
        for a in reversed(emitted):
            ends[a] = end
            if a in leaders:
                end = a
        codes = {}
        self.seg = (emitted, units, leaders)
        for k, a in enumerate(emitted):
            self.out = []
            if img.ins(a).is_transfer():
                self.transfer(a)
            else:
                left = (ends[a] - a) // 4
                cont = ("RC_SEG(%s, %d, %d)" % (hx(a + 4), segment_ticks(emitted, k + 1, units, leaders), left - 1)
                        if left > 1 else "")
                self.plain(a, left, cont)
            codes[a] = self.out
        # the guest registers it uses (locals) and writes (stored back)
        used, wr = set(), set()
        for a in f.addrs:
            i = img.ins(a)
            used.update(i.reads())
            wr.update(i.writes())
        written = sorted(wr)
        read_only = sorted(used - wr)
        out = ["RC_FN %s(n64_machine *m_)\n{\n" % self.name, "    RC_ENTER(%s);\n" % cpi_name]
        for regs, q in ((written, ""), (read_only, "const ")):
            for k in range(0, len(regs), 6):
                out.append("    %suint64_t %s;\n" % (q, ", ".join("r%d = h->cpu.gpr[%d]" % (r, r)
                                                                  for r in regs[k:k + 6])))
        out.append("    switch ((uint32_t)h->cpu.pc) {\n")
        out += ["    case %s: goto L_%08X;\n" % (hx(a), a) for a in emitted if a in leaders]
        # the other instructions (inside blocks): how many to their block's end, from a table
        tab = [0] * len(f.addrs)
        for a in emitted:
            if a not in leaders:
                tab[(a - f.start) // 4] = (ends[a] - a) // 4
        while tab and not tab[-1]:
            tab.pop()
        if tab:
            rows = [", ".join(str(v) for v in tab[k:k + 40]) for k in range(0, len(tab), 40)]
            out.append("    default: {\n        static const uint8_t mid_[%d] = {%s};\n"
                       % (len(tab), (",\n" + " " * 12).join(rows)))
            out.append("        RC_MID_TABLE(%s, mid_);\n        RC_NOT_COVERED();\n    }\n    }\n" % hx(f.start))
        else:
            out.append("    default: RC_NOT_COVERED();\n    }\n")
        seg_start = True
        for k, a in enumerate(emitted):
            i = img.ins(a)
            if a in leaders:
                out.append("L_%08X:%s\n" % (a, " /* %s */" % i.text() if comments else ""))
                seg_start = True
            elif comments:
                out.append("    /* %08X: %s */\n" % (a, i.text()))
            if seg_start:
                out.append("    RC_SEG(%s, %d, %d);\n" % (hx(a), segment_ticks(emitted, k, units, leaders),
                                                          (ends[a] - a) // 4))
            out.append("    RC_I();\n")
            out += codes[a]
            seg_start = any(d for _, d in units[a])
        out.append("    RC_EXIT(%s);\n" % hx(f.end))
        out.append("    RC_EPILOGUE_BEGIN();\n")
        out += ["    h->cpu.gpr[%d] = r%d;\n" % (r, r) for r in written]
        out.append("    RC_EPILOGUE_END();\n}\n")
        return "".join(out)

    def unit(self, i, cpi):
        """(ticks at most, dynamic) of instruction i in a segment (« blocks and event checks » below)."""
        return cpi + MUL_TICKS.get(i.name, 0), False

    def block_leaders(self, emitted, units):
        """The resume points: the function's start, jump targets, the instruction after a transfer (return points,
        fall-through) and after one always handed to the interpreter (COP0, SYSCALL, ...); then one every MAX_BLOCK
        instructions."""
        img = self.img
        em = set(emitted)
        lead = {self.f.start} | (self.targets & em)
        for k, a in enumerate(emitted[:-1]):
            i = img.ins(a)
            if i.is_transfer() or i.cls == "interp":
                lead.add(emitted[k + 1])
            if i.is_transfer() and a + 8 in em:
                lead.add(a + 8)  # the fall-through (also when the slot is emitted on its own: a branch targets it)
        run = 0
        for a in emitted:
            if a in lead or self.resume_all or run >= MAX_BLOCK:
                lead.add(a)
                run = 0
            run += len(units[a])
        return lead


# ---- blocks and event checks ----------------------------------------------------------------------------------------
#
# The interpreter checks `now < stop_at` and the interrupt line before every instruction. Lifted code stops before the
# same instruction with one check per block (more after calls into the core, below), rather than a label, a test and
# an exit per instruction (what made the C slow to compile). A block starts at a resume point (block_leaders) and runs
# to the next one, MAX_BLOCK instructions at most. Its instructions before the last cost static ticks at most (their
# fetch, multiply / divide latencies; cached RDRAM accesses cost nothing more), so `now + those < stop` at its start
# means no check inside would stop. RC_SEG is also the exact check of its first instruction: due now (or an interrupt
# pending: the stop is 0 then), leave for the dispatcher; due inside the block, the interpreter runs the rest of it
# (N64_RC_EXIT_INTERP, rc_hint = how many instructions). Entered inside a block (an interrupt's ERET returns there),
# the function answers the same from its mid_ table.
#
# ---- memory accesses ----
#
# Cached RDRAM (KSEG0, aligned, MI repeat mode off, not into this function's own code) inline, with static ticks.
# For an access whose address the lifter does not know, anything else is handed to the interpreter with the rest of
# its block (RC_*_RAM + RC_HAND: small code, no call into the core). An access to a known constant address off cached
# RDRAM (io_accesses: the drivers' RCP registers, uncached RDRAM, the cartridge), COP1 arithmetic and CTC1 go through
# the core (n64_rc_read / write / ..., n64_fpu.c: the interpreter's translation, costs and devices; an access that
# would raise is handed to the interpreter, nothing done). The core may charge ticks, raise an interrupt, move the
# stop, write into this code: after such a call the rest of the block is checked again from the next instruction (an
# RC_SEG for it, the « cont » of plain()), so the polling loops of drivers stay native and exact. In a delay slot an
# access goes through the core (no hand-off: the slot's branch is done) and needs no re-check: the next instruction is
# a branch target or the fall-through, a resume point with a check of its own, or outside the function.

MAX_BLOCK = 12  # instructions per block at most (an event in a block makes the interpreter run the rest of it)
MUL_TICKS = {"mult": 8, "multu": 8, "div": 70, "divu": 70, "dmult": 14, "dmultu": 14, "ddiv": 134, "ddivu": 134}
SIZES = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "lwu": 4, "ld": 8, "lwl": 4, "lwr": 4, "ldl": 8, "ldr": 8,
         "sb": 1, "sh": 2, "sw": 4, "sd": 8, "swl": 4, "swr": 4, "sdl": 8, "sdr": 8}


def is_cached_rdram(addr):
    return 0x80000000 <= addr < 0x80800000


def io_accesses(func):
    """The memory accesses of func whose address is a known constant off cached RDRAM (the drivers' RCP registers,
    uncached RDRAM, the cartridge): the base register's value comes from LUI (then ADDIU / DADDIU / ORI on it)
    earlier in the function, in address order, with no other write to it in between. A heuristic for speed only: an
    access it misses is handed to the interpreter when it is not to cached RDRAM, one it takes wrongly goes through
    the core; both are exact."""
    img, known, small, io = func.image, {}, set(), set()
    for a in func.addrs:
        i = img.ins(a)
        if i.cls in ("load", "store", "fmem") and i.rs in known:
            if not is_cached_rdram((known[i.rs] + i.simm) & 0xFFFFFFFF):
                io.add(a)
        n = i.name
        if n == "lui":
            known[i.rt] = (i.imm << 16) & 0xFFFFFFFF
            small.discard(i.rt)
            continue
        if n in ("addiu", "daddiu", "ori") and i.rs in known and i.rt == i.rs:
            known[i.rt] = (known[i.rs] + i.simm) & 0xFFFFFFFF if n != "ori" else known[i.rs] | i.imm
            continue
        if n in ("addu", "daddu", "or") and i.rd:
            # a 64 KB-aligned base plus an index masked by ANDI: the index stays inside the base's region (the
            # region decides IO or not; the low bits only matter to the core)
            base = next((known[x] for x, y in ((i.rs, i.rt), (i.rt, i.rs))
                         if x in known and not known[x] & 0xFFFF and y in small), None)
            small.discard(i.rd)
            if base is not None:
                known[i.rd] = base
                continue
            known.pop(i.rd, None)
            continue
        for r in i.writes():
            known.pop(r, None)
            small.discard(r)
        if n == "andi":
            small.add(i.rt)
    return io


def segment_ticks(emitted, k, units, leaders):
    """The static ticks, at most, of the segment starting at emitted[k] before its last instruction."""
    costs = []
    while k < len(emitted):
        for cyc, dyn in units[emitted[k]]:
            costs.append(cyc)
            if dyn:
                return sum(costs[:-1])
        k += 1
        if k < len(emitted) and emitted[k] in leaders:
            break
    return sum(costs[:-1])


__all__ = ["FnLifter", "hx", "is_direct"]
