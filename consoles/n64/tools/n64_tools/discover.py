"""Function discovery for N64 code without symbols (XBA-169).

Code lives in "images": the segments the boot loads (the classic boot megabyte or libdragon's ELF PT_LOAD segments,
rom.py), segments found copied by PI DMA with constant arguments (dma.py), and blobs of code the runtime recorded
from RDRAM (overlays, decompressed or TLB-mapped code: the discovered file of n64_recomp.h). For each image:

1. Seeds: the boot PC, the exception vectors (0x80000000 / 0x80000080 / 0x80000100 / 0x80000180 when an image holds
   them: libdragon loads its vectors there), the addresses of the discovered file, then JAL / BAL targets and
   tail-call J targets found while walking functions, code addresses built in registers (LUI + ADDIU / ORI /
   DADDIU), jump tables (LW / JR through a table bound by SLTIU), and code pointers in data (aligned words pointing at
   a plausible function start: a stack frame setup, or the first instruction after a JR ra / J / ERET and its slot,
   padding NOPs allowed).
2. Extent of a function: a linear walk from its start that ends after the delay slot of an unconditional transfer
   (J, JR, B, ERET) once no branch of the function points further, or at the next known start (or at a reserved
   encoding).
3. Linear sweep: compiler output is contiguous, so the gaps the recursive pass left between functions inside the
   text it reached are swept: each run of valid code after padding is a function (its calls are explored in turn).
   Past the last function reached, and over a gap larger than GAP_MAX between two functions reached (code reached
   only through a function pointer the scan missed is rarely that long: such a gap is data, e.g. the audio banks or
   compressed files that follow a game's boot segment in the boot megabyte), the sweep goes on only while candidates
   look like functions (a plausible start, valid instructions, a return), so data is not taken for code.
   An entry runs only when its bytes are in memory, but each one the runtime finds in a page costs a content check,
   so implausible code is left out everywhere: a function (other than a seed: the boot PC, the vectors, recorded
   code) whose body holds an instruction compilers do not emit (implausible(): LL / SC, the immediate traps, COP0
   other than the moves, TLB operations and ERET, a write to r0) is not taken, nor what it calls. This is what keeps a JAL
   of the boot segment into an overlay's run-time address (which holds data in the boot image) from pulling the data
   there in as code.
4. RSP microcode is data: it is never in an executable image of the CPU, and records are only made of code the CPU
   ran, so it is never lifted.
"""

import struct

from .isa import RA, SP, decode

MAX_TABLE = 512
MAX_BODY = 1000  # instructions per lifted C function at most (compile time grows faster than linearly with it)
PAGE = 4096
VECTORS = (0x80000000, 0x80000080, 0x80000100, 0x80000180)
ERET = 0x42000018


def is_direct(addr):
    return 0x80000000 <= addr < 0xC0000000


class Image:
    """Bytes of code at a virtual address (big-endian)."""

    def __init__(self, base, data, origin, executable=True):
        self.base = base & 0xFFFFFFFF
        self.data = bytes(data[: len(data) & ~3])
        self.end = self.base + len(self.data)
        self.origin = origin  # "boot", "elf", "dma", "ram"
        self.executable = executable
        self._cache = {}

    def contains(self, addr):
        return self.base <= addr < self.end and not (addr & 3)

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def ins(self, addr):
        i = self._cache.get(addr)
        if i is None:
            i = self._cache[addr] = decode(self.word(addr), addr)
        return i

    def words(self, start, end):
        return list(struct.unpack_from(">%dI" % ((end - start) // 4), self.data, start - self.base))


class Function:
    __slots__ = ("image", "start", "end", "name", "kind", "hash")

    def __init__(self, image, start, end):
        self.image = image
        self.start = start
        self.end = end
        self.name = None
        self.kind = "code"
        self.hash = recomp_hash(image.words(start, end))

    @property
    def addrs(self):
        return range(self.start, self.end, 4)


def recomp_hash(words):
    """n64_recomp_hash (n64_recomp.h): FNV-1a 64 over 32-bit words (big-endian values)."""
    h = 0xCBF29CE484222325
    for w in words:
        h = ((h ^ w) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


BAD = ("reserved", "cop3", "cop2", "lwc2", "swc2", "ldc2", "sdc2")
GAP_MAX = 0x10000  # bytes: a larger gap between functions reached is swept strictly (3. above)
RARE = {"ll", "lld", "sc", "scd", "tgei", "tgeiu", "tlti", "tltiu", "teqi", "tnei"}
COP0_OK = {0, 1, 4, 5}  # MFC0 DMFC0 MTC0 DMTC0
COP0_CO = {0x01, 0x02, 0x06, 0x08, 0x18}  # TLBR TLBWI TLBWR TLBP ERET


def implausible(i):
    """An instruction compilers do not emit (data decoded as code): see 3. above."""
    if i.name in BAD or i.name in RARE:
        return True
    if i.name == "cop0":
        sub = (i.word >> 21) & 31
        return not (sub in COP0_OK or (sub == 0x10 and (i.word & 63) in COP0_CO and not (i.word >> 6) & 0x7FFFF))
    if i.cls in ("alu", "ovf") and i.word != 0 and i.name != "sync":
        d = i.rd if (i.word >> 26) == 0 else i.rt
        return d == 0
    return False


def plausible_code(img, addr, n=4):
    """Does addr look like an instruction stream (no reserved encoding in its first instructions)?"""
    for k in range(n):
        a = addr + 4 * k
        if not img.contains(a):
            return k > 0
        if img.ins(a).name in BAD:
            return False
    return True


def ends_function(i):
    """An instruction after whose delay slot control never falls through (J, JR, B), or ERET (no slot)."""
    return i.word == ERET or (i.is_transfer() and i.is_unconditional() and not i.is_call())


def strong_end(i, addr):
    """A function's last transfer for sure: JR ra, ERET, or a J far away (a tail call). Not B / JR through a
    register: those end switch cases and loops inside functions too."""
    if i.word == ERET or (i.name == "jr" and i.rs == RA):
        return True
    return i.name == "j" and abs(i.target - addr) >= 0x4000


def after_end(img, addr):
    """Is addr the first instruction after the end of a function (its slot, then up to 8 padding NOPs)?"""
    a = addr - 4
    for _ in range(10):
        if not img.contains(a):
            return False
        i = img.ins(a)
        if i.word == ERET:
            return True
        if img.contains(a - 4) and strong_end(img.ins(a - 4), a - 4) and img.ins(a - 4).word != ERET:
            return True
        if i.word != 0:
            return False
        a -= 4
    return False


def looks_like_start(img, addr):
    """A pointer target that is likely a function: a stack frame setup, or right after the end of a function."""
    if not img.contains(addr) or not plausible_code(img, addr):
        return False
    i = img.ins(addr)
    if i.name in ("addiu", "daddiu") and i.rt == SP and i.rs == SP and i.simm < 0:
        return True
    return after_end(img, addr)


class Discovery:
    def __init__(self, img, seeds, data_images=(), sweep=True):
        self.img = img
        self.data_images = [d for d in data_images if d is not img]
        self.seeds = set(a for a in seeds if img.contains(a))
        self.starts = set(self.seeds)
        self.sweep = sweep
        self.cont = set()  # code after an internal unconditional branch that the sweep found: part of that function
        self.cont_owner = {}
        self.rejected = set()  # starts whose body is implausible (data)
        self.tables = {}  # jump table address -> entries
        self.stats = {"seeds": len(self.seeds), "jal": 0, "tail": 0, "pointers": 0, "jump_tables": 0, "swept": 0}

    # ---- one function's walk ----
    def _walk(self, start, limit, found):
        """Returns the end (exclusive) of the function at start; adds call targets etc. to found."""
        img = self.img
        furthest = start
        pc = start
        his = {}  # register -> LUI immediate
        while pc < limit and img.contains(pc):
            i = img.ins(pc)
            if i.name in BAD:
                return pc
            if i.word == ERET and furthest <= pc:
                return pc + 4
            if i.cls == "branch":
                t = i.target
                if i.is_call():
                    if img.contains(t):
                        found.add(("jal", t))
                elif start <= t < limit:
                    furthest = max(furthest, t)
            elif i.cls == "jump":
                t = i.target
                if i.name == "jal":
                    if img.contains(t):
                        found.add(("jal", t))
                elif (start <= t < limit and (t <= pc or t <= furthest + 64 or t < pc + 256)
                      and not (t >= pc + 8 and self._nops(pc + 8, t))):
                    furthest = max(furthest, t)
                elif img.contains(t):
                    found.add(("tail", t))  # a J to the code right after its slot (and padding) is a tail call
            elif i.name == "jr" and i.rs != RA:
                for t in self._jump_table(start, pc, i.rs):
                    if start <= t < limit:
                        furthest = max(furthest, t)
            if i.name == "lui":
                his[i.rt] = i.imm
            elif i.name in ("addiu", "daddiu", "ori") and i.rs in his:
                # LUI rX, hi ... ADDIU / ORI rY, rX, lo: an address built in a register (a function pointer?)
                v = ((his[i.rs] << 16) + (i.imm if i.name == "ori" else i.simm)) & 0xFFFFFFFF
                if looks_like_start(img, v):
                    found.add(("ptr", v))
            for d in i.writes():
                if not (i.name == "lui" and d == i.rt):
                    his.pop(d, None)
            if ends_function(i) and furthest <= pc + 4 and not self._continues(pc + 8):
                end = pc + 8
                return end if img.contains(pc + 4) else pc + 4
            pc += 4
            if furthest < pc and pc < limit and img.contains(pc) and self._after_noreturn(start, pc):
                return pc
        end = min(pc, limit)
        # never cut between a branch and its delay slot
        if end - 4 >= start and img.contains(end - 4) and img.ins(end - 4).is_transfer() and img.contains(end):
            end += 4
        return end

    def _continues(self, a):
        """Is there a continuation at a (after padding NOPs)?"""
        for k in range(9):
            if a + 4 * k in self.cont:
                return True
            if not self.img.contains(a + 4 * k) or self.img.word(a + 4 * k) != 0:
                return False
        return False

    def _nops(self, a, b):
        """Only NOPs (padding) in [a, b)?"""
        return all(self.img.word(x) == 0 for x in range(a, b, 4))

    def _after_noreturn(self, start, pc):
        """Does a new function start at pc, after a call that does not return (or a SYSCALL / BREAK) at the end of
        the previous one: the call, its slot, then padding NOPs up to an aligned address, or a stack frame setup
        right after the slot?"""
        img = self.img
        if pc - 8 < start:
            return False
        q = pc - 4
        while q > start and img.word(q) == 0:
            q -= 4
        i = img.ins(q)
        if i.is_call() and q + 4 < pc:
            pad = (pc - (q + 8)) // 4
        elif q - 4 >= start and img.ins(q - 4).is_call():
            pad = (pc - (q + 4)) // 4
        elif i.name in ("syscall", "break"):
            return (pc - (q + 4)) // 4 >= 2 and pc % 16 == 0
        else:
            return False
        if pad >= 1:
            return pc % 16 == 0
        n = img.ins(pc)
        return n.name in ("addiu", "daddiu") and n.rt == SP and n.rs == SP and n.simm < 0

    def _jump_table(self, start, pc, reg):
        """Targets of a JR reg through a table (LUI hi ; ADDU ; LW reg, lo(base) ; JR reg), or []."""
        img = self.img
        lw = None
        a = pc - 4
        while a >= max(start, pc - 64):
            i = img.ins(a)
            if i.name == "lw" and i.rt == reg:
                lw = (a, i)
                break
            if reg in i.writes():
                return []
            a -= 4
        if not lw:
            return []
        base, off = lw[1].rs, lw[1].simm
        srcs = {base}
        hi = None
        bound = None
        a = lw[0] - 4
        while a >= max(start, lw[0] - 80):
            i = img.ins(a)
            if i.name == "lui" and i.rt in srcs and hi is None:
                hi = i.imm
            elif i.name in ("addu", "daddu") and i.rd in srcs:
                srcs |= {i.rs, i.rt}
            elif i.name in ("addiu", "daddiu") and i.rt in srcs and i.rs == i.rt and hi is None:
                off += i.simm  # LUI r, hi ; ADDIU r, r, lo ; ADDU ; LW 0(r)
            elif i.name == "sltiu" and bound is None:
                bound = i.imm
            a -= 4
        if hi is None:
            return []
        table = ((hi << 16) + off) & 0xFFFFFFFF
        n = bound if bound and bound <= MAX_TABLE else MAX_TABLE
        src = img if img.contains(table) else next((d for d in self.data_images if d.contains(table)), None)
        if src is None:
            return []
        out = []
        for k in range(n):
            e = table + 4 * k
            if not src.contains(e):
                break
            t = src.word(e)
            if not img.contains(t) or t < start:
                break
            out.append(t)
        if out:
            if table not in self.tables:
                self.stats["jump_tables"] += 1
            self.tables[table] = len(out)
        return out

    def _pointer_scan(self):
        img = self.img
        out = set()
        in_table = set()
        for t, n in self.tables.items():
            in_table.update(range(t, t + 4 * n, 4))
        for src in [img] + self.data_images:
            vals = struct.unpack(">%dI" % (len(src.data) // 4), src.data)
            for k, v in enumerate(vals):
                if src is img and src.base + 4 * k in in_table:
                    continue  # jump table entries are case labels, not functions
                if v & 3 or not img.contains(v) or v in self.starts:
                    continue
                if looks_like_start(img, v):
                    out.add(v)
        return out

    def plausible_body(self, start, end):
        """No implausible instruction in [start, end)."""
        return not any(implausible(self.img.ins(a)) for a in range(start, end, 4) if self.img.contains(a))

    def _explore(self, work, done):
        img = self.img
        new = 0
        while work:
            s = work.pop()
            if s in done:
                if s in self.rejected:
                    self.starts.discard(s)  # found again (a pointer in data): still data
                continue
            done.add(s)
            found = set()
            end = self._walk(s, img.end, found)
            if s not in self.seeds and not self.plausible_body(s, end):
                self.starts.discard(s)  # data taken for code: neither a function nor a source of calls
                self.rejected.add(s)
                self.stats["implausible"] = self.stats.get("implausible", 0) + 1
                continue
            for kind, t in found:
                if t not in self.starts and plausible_code(img, t):
                    self.starts.add(t)
                    self.stats[kind if kind in ("jal", "tail") else "pointers"] += 1
                    work.append(t)
                    new += 1
        return new

    def _extents(self):
        starts = sorted(self.starts)
        out = []
        for k, s in enumerate(starts):
            limit = starts[k + 1] if k + 1 < len(starts) else self.img.end
            out.append((s, self._walk(s, limit, set())))
        return out

    def _internal_branch_end(self, fstart, end):
        """Did the code [fstart, end) end with an unconditional branch back into itself (B / J, then its slot) or a
        computed jump (JR through a register other than ra: a jump table)? Padding NOPs after it are skipped."""
        img = self.img
        while end - 4 > fstart and img.word(end - 4) == 0 and img.contains(end - 12) and \
                not img.ins(end - 8).is_transfer():
            end -= 4
        if end - 8 < fstart or not img.contains(end - 8):
            return False
        i = img.ins(end - 8)
        if i.name == "jr" and i.rs != RA:
            return True
        return (i.is_transfer() and i.is_unconditional() and not i.is_call() and i.target is not None
                and fstart <= i.target < end)

    def _strong_start(self, a):
        """A function start for sure: a stack frame setup, or after a return (JR ra), an ERET or a J out."""
        return looks_like_start(self.img, a)

    def _sweep_gap(self, a, b, strict, fstart=None):
        """Functions in [a, b) (b exclusive): runs of valid code after padding. fstart: the function that ends at a.
        Code right after a function that jumps through a table we could not read (JR through a computed register),
        when that function's code so far ended with a branch back into itself and the code does not look like a
        function start, continues that function (the cases of its switch): it becomes a continuation, not a start."""
        img = self.img
        out = []
        owner = fstart
        while a < b:
            c = a
            while fstart is not None and c < b and img.word(c) == 0 and c - a < 32:
                c += 4
            if (fstart is not None and c < b and img.word(c) != 0 and self._internal_branch_end(fstart, c)
                    and self._computed_jump(owner, c) and not self._strong_start(c) and plausible_code(img, c, 2)):
                a = c
                self.cont.add(a)
                self.cont_owner[a] = owner
                end = self._walk(a, b, set())
                if end <= a:
                    break
                a = end
                continue
            fstart = None
            while a < b and img.word(a) == 0:
                a += 4
            if a >= b:
                break
            if not plausible_code(img, a, 2) or (strict and not looks_like_start(img, a)):
                if strict:
                    break
                a += 4
                continue
            end = self._walk(a, b, set())
            if end <= a:
                a += 4
                continue
            if not self.plausible_body(a, end):
                if strict:
                    break
                a += 4
                continue
            if strict:
                body = [img.ins(x) for x in range(a, end, 4)]
                if not any(ends_function(i) for i in body):
                    break
            out.append(a)
            fstart = owner = a
            a = end
        return out

    def _computed_jump(self, start, end):
        """Does [start, end) hold a JR through a register other than ra whose jump table was not found?"""
        img = self.img
        start = self.cont_owner.get(start, start)
        for a in range(start, end, 4):
            i = img.ins(a)
            if i.name == "jr" and i.rs != RA and not self._jump_table(start, a, i.rs):
                return True
        return False

    def _sweep(self):
        """The linear sweep (3. above). Returns the starts it adds."""
        img = self.img
        ext = self._extents()
        if not ext:
            return set()
        text_end = max(e for _, e in ext)
        added = set()
        prev, pstart = img.base, None
        ncont = len(self.cont)
        for s, e in ext:
            if s >= text_end:
                break
            if s > prev:
                added.update(self._sweep_gap(prev, s, strict=s - prev > GAP_MAX, fstart=pstart))
            if e > prev:
                prev, pstart = e, self.cont_owner.get(s, s)
        if prev < img.end:
            added.update(self._sweep_gap(prev, img.end, strict=True))
        added -= self.starts
        if not added and len(self.cont) != ncont:
            added = {None}  # continuations only: extents changed, sweep again
        return added

    def _inner_seeds(self):
        """Recorded code (no sweep): the seeds inside the body of the function an earlier seed starts (the runtime
        records where the interpreter had to start: function starts, but also return points and branch targets).
        They are not starts of their own, which would cut that function into pieces each left through the
        dispatcher; the function's entry switch resumes there."""
        inner, end = set(), None
        for s in sorted(self.seeds):
            if end is not None and s < end:
                inner.add(s)
                continue
            end = self._walk(s, self.img.end, set())
        return inner

    def run(self):
        img = self.img
        done = set()
        if not self.sweep or img.origin == "ram":
            inner = self._inner_seeds()
            self.starts -= inner
            self.stats["inner_seeds"] = len(inner)
        # from the seeds first (this finds the jump tables), then from the code pointers in data, then the sweep
        self._explore(sorted(self.starts), done)
        ptrs = self._pointer_scan()
        self.stats["pointers"] += len(ptrs)
        self.starts |= ptrs
        self._explore(sorted(ptrs), done)
        if self.sweep and img.origin != "ram":
            for _ in range(16):
                swept = self._sweep()
                if not swept:
                    break
                swept.discard(None)
                self.stats["swept"] += len(swept)
                self.starts |= swept
                self._explore(sorted(swept), done)
        funcs = []
        for s, end in self._extents():
            if end <= s:
                continue
            for a, b in self._pieces(s, end):
                funcs.append(Function(img, a, b))
        return funcs

    def _pieces(self, start, end):
        """A body longer than MAX_BODY instructions is cut in pieces (one C function each, for the compiler's sake),
        and a TLB-mapped one at each 4 KB page (one translation per entry: n64_recomp.h), never between a branch and
        its delay slot unless the page forces it; a piece falls through to the next one via the dispatcher."""
        out = []
        a = start
        mapped = not is_direct(start)
        while a < end:
            b = min(end, a + MAX_BODY * 4)
            if mapped:
                b = min(b, (a & ~(PAGE - 1)) + PAGE)
            if b < end and not (mapped and b == (a & ~(PAGE - 1)) + PAGE):
                while b > a + 8 and self.img.ins(b - 4).is_transfer():
                    b -= 4
            if b < end:
                self.stats["cut"] = self.stats.get("cut", 0) + 1
            out.append((a, b))
            a = b
        return out


def merge_records(records):
    """Recorded RAM code (addr, bytes) merged into blobs: records that overlap with the same bytes become one.
    Returns [(base, bytes, [seed addresses])]."""
    blobs = []
    for addr, data in sorted(records, key=lambda r: (r[0], -len(r[1]))):
        merged = False
        for b in blobs:
            base, buf, seeds = b
            lo, hi = max(base, addr), min(base + len(buf), addr + len(data))
            if lo <= hi and buf[lo - base:hi - base] == data[lo - addr:hi - addr]:
                if not is_direct(addr) and (min(base, addr) >> 12) != ((max(base + len(buf), addr + len(data)) - 1)
                                                                      >> 12):
                    continue  # mapped code: blobs stay inside one page (another page may map anywhere)
                nb = min(base, addr)
                ne = max(base + len(buf), addr + len(data))
                out = bytearray(ne - nb)
                out[addr - nb:addr - nb + len(data)] = data
                out[base - nb:base - nb + len(buf)] = buf
                b[0], b[1] = nb, bytes(out)
                seeds.append(addr)
                merged = True
                break
        if not merged:
            blobs.append([addr, bytes(data), [addr]])
    return [tuple(b) for b in blobs]
