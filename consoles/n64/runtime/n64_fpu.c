/* VR4300 COP1 (N-3, XBA-171): single / double / word / long formats, the FR = 0 / 1 register views, FCSR rounding
 * modes, flags, enables and causes, the floating-point exception, and the cases the VR4300 leaves to software (the
 * unimplemented operation, cause E, which cannot be masked, and which then is the only cause): denormal or quiet-NaN
 * operands, denormal or underflowing results unless FS flushes them (and underflow / inexact are not enabled),
 * float -> integer conversions out of range (or from NaN / infinity), 64-bit integer -> float conversions beyond
 * 2^55. A signaling NaN operand is an invalid operation (V); compares raise V for a signaling NaN, or any NaN with the
 * signaling conditions. NaNs use the MIPS legacy encoding (top mantissa bit set = signaling) and NaN results are the
 * VR4300's default quiet NaN (0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF), as n64-systemtest measures the hardware.
 *
 * The host FPU does the arithmetic (IEEE 754 binary32 / binary64 with <fenv.h> rounding modes and exception flags:
 * x86-64 SSE, AArch64), so results are bit-exact for the operations IEEE defines. The source file is built with
 * -frounding-math so the compiler keeps the operations between fesetround / fetestexcept. */
#include "n64_internal.h"

#include <fenv.h>
#include <math.h>
#include <string.h>

#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

#define FS(i) (((i) >> 11) & 31)
#define FT(i) (((i) >> 16) & 31)
#define FD(i) (((i) >> 6) & 31)

#define NAN_S 0x7FBFFFFFu
#define NAN_D 0x7FF7FFFFFFFFFFFFull

/* ---- Register views -------------------------------------------------------------------------------------------- */

static inline bool fr(const n64_cpu *c) { return (c->cop0[C0_STATUS] & SR_FR) != 0; }

uint32_t n64_fpu_read_s(n64_cpu *c, int r)
{
    if (fr(c))
        return (uint32_t)c->fpr[r];
    return (r & 1) ? (uint32_t)(c->fpr[r & ~1] >> 32) : (uint32_t)c->fpr[r];
}

void n64_fpu_write_s(n64_cpu *c, int r, uint32_t v)
{
    if (fr(c))
        c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | v;
    else if (r & 1)
        c->fpr[r & ~1] = (c->fpr[r & ~1] & 0xFFFFFFFFull) | (uint64_t)v << 32;
    else
        c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | v;
}

/* The operand view of the arithmetic and conversion instructions (as n64-systemtest measures it), which differs from
 * the moves' above when FR = 0: fs ignores its lowest bit, ft and fd name the physical 64-bit register as they are; a
 * single / word source is the low half of that register, a single / word result is written zero-extended to all of
 * it. With FR = 0 and the even registers compilers use, both views agree. */
static inline int fs_index(const n64_cpu *c, int r) { return fr(c) ? r : (r & ~1); }
static inline uint32_t src_s(const n64_cpu *c, int r) { return (uint32_t)c->fpr[fs_index(c, r)]; }
static inline uint64_t src_d(const n64_cpu *c, int r) { return c->fpr[fs_index(c, r)]; }
static inline void dst_s(n64_cpu *c, int r, uint32_t v) { c->fpr[r] = v; }
static inline void dst_d(n64_cpu *c, int r, uint64_t v) { c->fpr[r] = v; }

uint64_t n64_fpu_read_d(n64_cpu *c, int r) { return c->fpr[fr(c) ? r : (r & ~1)]; }

void n64_fpu_write_d(n64_cpu *c, int r, uint64_t v) { c->fpr[fr(c) ? r : (r & ~1)] = v; }

static inline float f_of(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static inline uint32_t b_of_f(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
static inline double d_of(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static inline uint64_t b_of_d(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

/* ---- Classification -------------------------------------------------------------------------------------------- */

enum { K_NORMAL, K_ZERO, K_DENORM, K_INF, K_QNAN, K_SNAN };

static int class_s(uint32_t b)
{
    uint32_t e = (b >> 23) & 0xFF, man = b & 0x7FFFFF;
    if (e == 0)
        return man ? K_DENORM : K_ZERO;
    if (e == 0xFF)
        return man == 0 ? K_INF : (man & 0x400000) ? K_SNAN : K_QNAN;
    return K_NORMAL;
}

static int class_d(uint64_t b)
{
    uint64_t e = (b >> 52) & 0x7FF, man = b & 0xFFFFFFFFFFFFFull;
    if (e == 0)
        return man ? K_DENORM : K_ZERO;
    if (e == 0x7FF)
        return man == 0 ? K_INF : (man & 0x8000000000000ull) ? K_SNAN : K_QNAN;
    return K_NORMAL;
}

/* Cause bits an operand gives (arithmetic). */
static uint32_t in_cause(int k)
{
    switch (k) {
    case K_DENORM:
    case K_QNAN:
        return FPE_E;
    case K_SNAN:
        return FPE_V;
    default:
        return 0;
    }
}

/* ---- Host rounding / flags ------------------------------------------------------------------------------------- */

static const int host_rm[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};

static inline void op_begin(const n64_cpu *c)
{
    feclearexcept(FE_ALL_EXCEPT);
    if (c->fcsr & 3)
        fesetround(host_rm[c->fcsr & 3]);
}

static inline uint32_t op_end(const n64_cpu *c)
{
    int x = fetestexcept(FE_ALL_EXCEPT);
    if (c->fcsr & 3)
        fesetround(FE_TONEAREST);
    uint32_t cause = 0;
    if (x & FE_INEXACT)
        cause |= FPE_I;
    if (x & FE_UNDERFLOW)
        cause |= FPE_U;
    if (x & FE_OVERFLOW)
        cause |= FPE_O;
    if (x & FE_DIVBYZERO)
        cause |= FPE_Z;
    if (x & FE_INVALID)
        cause |= FPE_V;
    return cause;
}

/* Sets the cause field; raises the exception (returns false: the destination is not written) when a cause is
 * enabled or is E; else adds the causes to the flags. */
static bool finish(n64_machine *m, uint32_t cause)
{
    n64_cpu *c = &m->cpu;
    if (cause & FPE_E)
        cause = FPE_E; /* the unimplemented operation reports alone */
    c->fcsr = (c->fcsr & ~(0x3Fu << FCSR_CAUSE_SHIFT)) | (cause << FCSR_CAUSE_SHIFT);
    uint32_t en = (c->fcsr >> FCSR_ENABLES_SHIFT) & 0x1F;
    if (cause & (en | FPE_E)) {
        n64_cpu_exception(m, EXC_FPE, 0);
        return false;
    }
    c->fcsr |= (cause & 0x1F) << FCSR_FLAGS_SHIFT;
    return true;
}

/* Result checks: NaN -> default NaN; denormal / underflow -> E, or flushed when FS (to zero or to the smallest normal,
 * by the rounding mode). */
static uint32_t out_s(n64_cpu *c, uint32_t *b, uint32_t cause)
{
    int k = class_s(*b);
    if (k == K_QNAN || k == K_SNAN) {
        *b = NAN_S;
        return cause;
    }
    if (k == K_DENORM || (cause & FPE_U)) {
        /* Flushing needs FS and neither underflow nor inexact enabled; else the unimplemented operation. */
        if (!(c->fcsr & FCSR_FS) || (c->fcsr & ((FPE_U | FPE_I) << FCSR_ENABLES_SHIFT)))
            return FPE_E;
        bool neg = (*b >> 31) != 0;
        uint32_t rm = c->fcsr & 3;
        bool up = (rm == 2 && !neg) || (rm == 3 && neg);
        *b = (neg ? 0x80000000u : 0) | (up ? 0x00800000u : 0);
        return cause | FPE_U | FPE_I;
    }
    return cause;
}

static uint32_t out_d(n64_cpu *c, uint64_t *b, uint32_t cause)
{
    int k = class_d(*b);
    if (k == K_QNAN || k == K_SNAN) {
        *b = NAN_D;
        return cause;
    }
    if (k == K_DENORM || (cause & FPE_U)) {
        /* Flushing needs FS and neither underflow nor inexact enabled; else the unimplemented operation. */
        if (!(c->fcsr & FCSR_FS) || (c->fcsr & ((FPE_U | FPE_I) << FCSR_ENABLES_SHIFT)))
            return FPE_E;
        bool neg = (*b >> 63) != 0;
        uint32_t rm = c->fcsr & 3;
        bool up = (rm == 2 && !neg) || (rm == 3 && neg);
        *b = (neg ? 0x8000000000000000ull : 0) | (up ? 0x0010000000000000ull : 0);
        return cause | FPE_U | FPE_I;
    }
    return cause;
}

/* ---- FCSR ------------------------------------------------------------------------------------------------------ */

void n64_fpu_write_fcsr(n64_machine *m, uint32_t v)
{
    n64_cpu *c = &m->cpu;
    c->fcsr = v & 0x0183FFFFu;
    uint32_t cause = (c->fcsr >> FCSR_CAUSE_SHIFT) & 0x3F;
    uint32_t en = (c->fcsr >> FCSR_ENABLES_SHIFT) & 0x1F;
    /* The exception is taken while the next instruction is decoded: a COP1 / COP2 one leaves its number in Cause CE
     * (n64-systemtest "Fire exception through CTC1 (followed by MFC1)", cycle group). */
    if (cause & (en | FPE_E))
        n64_cpu_exception(m, EXC_FPE, n64_cpu_next_cop(m));
}

/* ---- Operations ------------------------------------------------------------------------------------------------ */

static bool compare(uint32_t cond, bool unordered, bool less, bool equal)
{
    return ((cond & 1) && unordered) || ((cond & 2) && equal) || ((cond & 4) && less);
}

/* Float -> integer conversion with a given rounding: 0 nearest even, 1 toward zero, 2 up, 3 down. */
static double round_by(double x, int rm)
{
    switch (rm) {
    case 1: return trunc(x);
    case 2: return ceil(x);
    case 3: return floor(x);
    default: return nearbyint(x); /* the host runs in round-to-nearest-even outside op_begin / op_end */
    }
}

/* fmt S or D value as a double (exact for S), its class; for conversions to integers. */
static void to_int(n64_machine *m, int fmt, uint32_t ins, int rm, bool wide)
{
    n64_cpu *c = &m->cpu;
    int k;
    double x;
    if (fmt == 16) {
        uint32_t b = src_s(c, FS(ins));
        k = class_s(b);
        x = (double)f_of(b);
    } else {
        uint64_t b = src_d(c, FS(ins));
        k = class_d(b);
        x = d_of(b);
    }
    uint32_t cause = 0;
    if (k == K_DENORM || k == K_INF || k == K_QNAN || k == K_SNAN) {
        finish(m, FPE_E);
        return;
    }
    double r = round_by(x, rm);
    if (wide) {
        if (r >= 9007199254740992.0 || r <= -9007199254740992.0) { /* 2^53 */
            finish(m, FPE_E);
            return;
        }
    } else if (r >= 2147483648.0 || r < -2147483648.0) {
        finish(m, FPE_E);
        return;
    }
    if (r != x)
        cause |= FPE_I;
    if (!finish(m, cause))
        return;
    if (wide)
        dst_d(c, FD(ins), (uint64_t)(int64_t)r);
    else
        dst_s(c, FD(ins), (uint32_t)(int32_t)r);
}

static void op_s(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    uint32_t fn = ins & 0x3F;
    uint32_t a = src_s(c, FS(ins)), b = (uint32_t)c->fpr[FT(ins)];
    int ka = class_s(a), kb = class_s(b);
    uint32_t r = 0, cause;
    volatile float fa = f_of(a), fb = f_of(b);
    float fr_;
    switch (fn) {
    case 0x00: case 0x01: case 0x02: case 0x03: /* ADD SUB MUL DIV */
        cause = in_cause(ka) | in_cause(kb);
        if (cause) {
            r = NAN_S;
            break;
        }
        op_begin(c);
        switch (fn) {
        case 0x00: fr_ = fa + fb; break;
        case 0x01: fr_ = fa - fb; break;
        case 0x02: fr_ = fa * fb; break;
        default: fr_ = fa / fb; break;
        }
        r = b_of_f(fr_);
        cause = op_end(c);
        cause = out_s(c, &r, cause);
        m->now += fn == 0x03 ? 56 : fn == 0x02 ? 6 : 4;
        break;
    case 0x04: /* SQRT */
        cause = in_cause(ka);
        if (cause) {
            r = NAN_S;
            break;
        }
        op_begin(c);
        fr_ = sqrtf(fa);
        r = b_of_f(fr_);
        cause = op_end(c);
        cause = out_s(c, &r, cause);
        m->now += 56;
        break;
    case 0x05: /* ABS */
        cause = in_cause(ka);
        r = cause ? NAN_S : a & 0x7FFFFFFFu;
        break;
    case 0x06: /* MOV.S copies the whole register, like MOV.D */
        c->fpr[FD(ins)] = c->fpr[fs_index(c, FS(ins))];
        return;
    case 0x07: /* NEG */
        cause = in_cause(ka);
        r = cause ? NAN_S : a ^ 0x80000000u;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        to_int(m, 16, ins, (int)(fn & 3), true);
        return;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        to_int(m, 16, ins, (int)(fn & 3), false);
        return;
    case 0x21: { /* CVT.D.S */
        cause = in_cause(ka);
        if (!finish(m, cause))
            return;
        dst_d(c, FD(ins), cause ? NAN_D : b_of_d((double)f_of(a)));
        return;
    }
    case 0x24: /* CVT.W.S */
        to_int(m, 16, ins, (int)(c->fcsr & 3), false);
        return;
    case 0x25: /* CVT.L.S */
        to_int(m, 16, ins, (int)(c->fcsr & 3), true);
        return;
    default:
        if (fn >= 0x30) { /* C.cond.S */
            bool un = ka >= K_QNAN || kb >= K_QNAN;
            cause = 0;
            if (un && ((fn & 8) || ka == K_SNAN || kb == K_SNAN))
                cause = FPE_V;
            bool t = compare(fn & 15, un, !un && fa < fb, !un && fa == fb);
            if (!finish(m, cause))
                return;
            c->fcsr = t ? (c->fcsr | FCSR_C) : (c->fcsr & ~FCSR_C);
            return;
        }
        finish(m, FPE_E); /* CVT.S.S and the reserved functions */
        return;
    }
    if (finish(m, cause))
        dst_s(c, FD(ins), r);
}

static void op_d(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    uint32_t fn = ins & 0x3F;
    uint64_t a = src_d(c, FS(ins)), b = c->fpr[FT(ins)];
    int ka = class_d(a), kb = class_d(b);
    uint64_t r = 0;
    uint32_t cause;
    volatile double fa = d_of(a), fb = d_of(b);
    double fr_;
    switch (fn) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        cause = in_cause(ka) | in_cause(kb);
        if (cause) {
            r = NAN_D;
            break;
        }
        op_begin(c);
        switch (fn) {
        case 0x00: fr_ = fa + fb; break;
        case 0x01: fr_ = fa - fb; break;
        case 0x02: fr_ = fa * fb; break;
        default: fr_ = fa / fb; break;
        }
        r = b_of_d(fr_);
        cause = op_end(c);
        cause = out_d(c, &r, cause);
        m->now += fn == 0x03 ? 114 : fn == 0x02 ? 12 : 4;
        break;
    case 0x04:
        cause = in_cause(ka);
        if (cause) {
            r = NAN_D;
            break;
        }
        op_begin(c);
        fr_ = sqrt(fa);
        r = b_of_d(fr_);
        cause = op_end(c);
        cause = out_d(c, &r, cause);
        m->now += 114;
        break;
    case 0x05:
        cause = in_cause(ka);
        r = cause ? NAN_D : a & 0x7FFFFFFFFFFFFFFFull;
        break;
    case 0x06:
        c->fpr[FD(ins)] = c->fpr[fs_index(c, FS(ins))];
        return;
    case 0x07:
        cause = in_cause(ka);
        r = cause ? NAN_D : a ^ 0x8000000000000000ull;
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        to_int(m, 17, ins, (int)(fn & 3), true);
        return;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        to_int(m, 17, ins, (int)(fn & 3), false);
        return;
    case 0x20: { /* CVT.S.D */
        cause = in_cause(ka);
        uint32_t rs = NAN_S;
        if (!cause) {
            op_begin(c);
            volatile float t = (float)fa;
            rs = b_of_f(t);
            cause = op_end(c);
            cause = out_s(c, &rs, cause);
        }
        if (finish(m, cause))
            dst_s(c, FD(ins), rs);
        return;
    }
    case 0x24:
        to_int(m, 17, ins, (int)(c->fcsr & 3), false);
        return;
    case 0x25:
        to_int(m, 17, ins, (int)(c->fcsr & 3), true);
        return;
    default:
        if (fn >= 0x30) {
            bool un = ka >= K_QNAN || kb >= K_QNAN;
            cause = 0;
            if (un && ((fn & 8) || ka == K_SNAN || kb == K_SNAN))
                cause = FPE_V;
            bool t = compare(fn & 15, un, !un && fa < fb, !un && fa == fb);
            if (!finish(m, cause))
                return;
            c->fcsr = t ? (c->fcsr | FCSR_C) : (c->fcsr & ~FCSR_C);
            return;
        }
        finish(m, FPE_E);
        return;
    }
    if (finish(m, cause))
        dst_d(c, FD(ins), r);
}

/* W (32-bit) and L (64-bit) integer sources: only CVT.S / CVT.D. */
static void op_int(n64_machine *m, uint32_t ins, bool wide)
{
    n64_cpu *c = &m->cpu;
    uint32_t fn = ins & 0x3F;
    int64_t v = wide ? (int64_t)src_d(c, FS(ins)) : (int64_t)(int32_t)src_s(c, FS(ins));
    if (fn != 0x20 && fn != 0x21) {
        finish(m, FPE_E);
        return;
    }
    if (wide && (v >= (1ll << 55) || v < -(1ll << 55))) {
        finish(m, FPE_E);
        return;
    }
    volatile int64_t vv = v;
    op_begin(c);
    uint32_t rs = 0;
    uint64_t rd_ = 0;
    if (fn == 0x20) {
        volatile float f = (float)vv;
        rs = b_of_f(f);
    } else {
        volatile double d = (double)vv;
        rd_ = b_of_d(d);
    }
    uint32_t cause = op_end(c);
    if (!finish(m, cause))
        return;
    if (fn == 0x20)
        dst_s(c, FD(ins), rs);
    else
        dst_d(c, FD(ins), rd_);
}

void n64_fpu_cop1(n64_machine *m, uint32_t ins)
{
    n64_cpu *c = &m->cpu;
    int rt = FT(ins), fs = FS(ins);
    switch ((ins >> 21) & 31) {
    case 0x00: /* MFC1 */
        if (rt)
            c->gpr[rt] = (uint64_t)(int64_t)(int32_t)n64_fpu_read_s(c, fs);
        return;
    case 0x01: /* DMFC1 */
        if (!c->ops64)
            break;
        if (rt)
            c->gpr[rt] = n64_fpu_read_d(c, fs);
        return;
    case 0x02: /* CFC1 */
        if (rt) {
            uint32_t v = fs == 31 ? c->fcsr : fs == 0 ? 0x00000A00u : 0;
            c->gpr[rt] = (uint64_t)(int64_t)(int32_t)v;
        }
        return;
    case 0x04: /* MTC1 */
        n64_fpu_write_s(c, fs, (uint32_t)c->gpr[rt]);
        return;
    case 0x05: /* DMTC1 */
        if (!c->ops64)
            break;
        n64_fpu_write_d(c, fs, c->gpr[rt]);
        return;
    case 0x06: /* CTC1 */
        if (fs == 31)
            n64_fpu_write_fcsr(m, (uint32_t)c->gpr[rt]);
        return;
    case 0x08: { /* BC1F BC1T BC1FL BC1TL */
        bool cond = (c->fcsr & FCSR_C) != 0;
        bool taken = (rt & 1) ? cond : !cond;
        int64_t off = (int64_t)(int16_t)ins;
        if (rt & 2) { /* likely */
            if (taken) {
                c->next_ds = true;
                c->npc = c->pc + (uint64_t)(off << 2);
            } else {
                c->pc = c->npc;
                c->npc += 4;
            }
        } else {
            c->next_ds = true;
            if (taken)
                c->npc = c->pc + (uint64_t)(off << 2);
        }
        return;
    }
    case 0x10:
        op_s(m, ins);
        return;
    case 0x11:
        op_d(m, ins);
        return;
    case 0x14:
        op_int(m, ins, false);
        return;
    case 0x15:
        op_int(m, ins, true);
        return;
    default:
        finish(m, FPE_E); /* reserved formats: unimplemented operation */
        return;
    }
    n64_cpu_exception(m, EXC_RI, 0);
}
