// Fused multiply-add for builds without the FMA instruction (tools/build_game.py --cpu v2). The guest's FMADD/FMLA
// family must round once, as the Switch does. Without -mfma, clang turns every fused multiply-add into a call to the C
// library's fma/fmaf, and glibc's versions for CPUs without FMA hold the floating-point environment around each one
// (feholdexcept, fesetround, feupdateenv): about 20 ns per fmaf and 40 ns per fma on a CPU that is otherwise idle, more
// than ten times the hardware's cost, and enough to bring a battle down to a few frames per second (issue #36).
// The functions here give the same correctly rounded result without touching the environment, and leave the rare
// operands they cannot settle cheaply (NaN, infinity, results near or below the smallest normal, extreme exponents)
// to the C library as before, so those keep exactly the result the v2 build had.
// simd.h SIMD_FMA calls them; with -mfma this file is empty and SIMD_FMA is the hardware instruction.
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef __FMA__

typedef float vf32 __attribute__((vector_size(16), may_alias));
typedef double vf64 __attribute__((vector_size(16), may_alias));
typedef double vf64x4 __attribute__((vector_size(32), may_alias));
typedef int64_t vs64x4 __attribute__((vector_size(32), may_alias));
typedef uint64_t vu64x4 __attribute__((vector_size(32), may_alias));

static inline uint64_t bits_d(double x) { uint64_t u; memcpy(&u, &x, sizeof u); return u; }
static inline double from_d(uint64_t u) { double x; memcpy(&x, &u, sizeof x); return x; }

// The C library's software fma/fmaf compute with intermediate values that can be subnormal, which FTZ|DAZ (set while
// the guest runs with FPCR.FZ, cpu.h rt_fpcr_sync) would flush and so spoil the result. The edge cases therefore run
// it with both off and apply them as the FMA instruction does: subnormal operands read as zero (DAZ), and a tiny result
// becomes zero of the same sign (FTZ). x86 calls a result tiny after rounding to full precision with an unbounded
// exponent, so one that lands on the smallest normal may still be tiny: recomputed with the smaller factor and the
// addend scaled by 2^64 (exact, and far from overflow when the result is that small), it is tiny when it rounds below
// the scaled smallest normal. The tests are on the bits: under DAZ a comparison sees a subnormal as zero.
// FENV_ACCESS keeps the calls apart and in their MXCSR settings.
#define CSR_DAZ 0x0040u
#define CSR_FTZ 0x8000u

static inline uint32_t bits_f(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }
static inline float from_f(uint32_t u) { float x; memcpy(&x, &u, sizeof x); return x; }
static inline float flush_f(float x) { uint32_t u = bits_f(x); return (u & 0x7f800000u) ? x : from_f(u & 0x80000000u); }
static inline double flush_d(double x) {
    uint64_t u = bits_d(x);
    return (u & 0x7ff0000000000000ull) ? x : from_d(u & 0x8000000000000000ull);
}

__attribute__((noinline)) static float fmaf_edge(float a, float b, float c) {
#pragma STDC FENV_ACCESS ON
    unsigned csr = _mm_getcsr();
    if (!(csr & (CSR_DAZ | CSR_FTZ))) return fmaf(a, b, c);
    if (csr & CSR_DAZ) a = flush_f(a), b = flush_f(b), c = flush_f(c);
    _mm_setcsr(csr & ~(CSR_DAZ | CSR_FTZ));
    float r = fmaf(a, b, c);
    if ((csr & CSR_FTZ) && (bits_f(r) & 0x7fffffffu) == 0x00800000u) {
        const float k = 0x1p64f;
        int sa = (bits_f(a) & 0x7fffffffu) <= (bits_f(b) & 0x7fffffffu);
        float t = fmaf(sa ? a * k : a, sa ? b : b * k, c * k);
        if ((bits_f(t) & 0x7fffffffu) < 0x00800000u + (64u << 23)) r = from_f(bits_f(r) & 0x80000000u);   // 2^-126 * 2^64
    }
    _mm_setcsr(csr);
    return csr & CSR_FTZ ? flush_f(r) : r;
}

__attribute__((noinline)) static double fma_edge(double a, double b, double c) {
#pragma STDC FENV_ACCESS ON
    unsigned csr = _mm_getcsr();
    if (!(csr & (CSR_DAZ | CSR_FTZ))) return fma(a, b, c);
    if (csr & CSR_DAZ) a = flush_d(a), b = flush_d(b), c = flush_d(c);
    _mm_setcsr(csr & ~(CSR_DAZ | CSR_FTZ));
    double r = fma(a, b, c);
    const uint64_t mag = 0x7fffffffffffffffull, minn = 0x0010000000000000ull;
    if ((csr & CSR_FTZ) && (bits_d(r) & mag) == minn) {
        const double k = 0x1p64;
        int sa = (bits_d(a) & mag) <= (bits_d(b) & mag);
        double t = fma(sa ? a * k : a, sa ? b : b * k, c * k);
        if ((bits_d(t) & mag) < minn + (64ull << 52)) r = from_d(bits_d(r) & ~mag);   // 2^-1022 * 2^64
    }
    _mm_setcsr(csr);
    return csr & CSR_FTZ ? flush_d(r) : r;
}

// Float: the product of two floats is exact in double, so a*b+c rounded to double and then to float is the fused
// result unless the double sum lands exactly halfway between two floats (low 29 bits 0x10000000) after rounding: then
// the second rounding can go the wrong way, and the sum's rounding error says which way is right. Directed rounding
// modes round correctly twice in a row, so only round-to-nearest needs the fix. The fast range is double exponents
// above the smallest float binade up to the largest one; below it a float result can be subnormal (its rounding
// position moves) or flushed by FTZ, above it the result overflows.
#define F_LO (0x3ff - 126)
#define F_HI (0x3ff + 127)
#define HALF_MASK 0x1fffffffull
#define HALF_BITS 0x10000000ull


// A subnormal operand also goes to the C library: whether the double conversion treats it as zero under DAZ (FPCR.FZ)
// must not decide the result.
static inline int sub_f(float x) { uint32_t u = bits_f(x); return (u & 0x7f800000u) == 0 && (u << 1) != 0; }

static inline float fmaf_one(float a, float b, float c) {
    if (__builtin_expect(sub_f(a) | sub_f(b) | sub_f(c), 0)) return fmaf_edge(a, b, c);
    double p = (double)a * (double)b;
    double s = p + (double)c;
    uint64_t u = bits_d(s);
    int e = (int)(u >> 52 & 0x7ff);
    if (__builtin_expect(e <= F_LO || e > F_HI, 0)) return fmaf_edge(a, b, c);
    if (__builtin_expect((u & HALF_MASK) == HALF_BITS, 0) && (_mm_getcsr() & 0x6000) == 0) {
        // Two-sum: err is exactly p + c - s.
        double bb = s - p;
        double err = (p - (s - bb)) + ((double)c - bb);
        if (err != 0) u += ((err > 0) == !(u >> 63)) ? 1 : -1;   // step toward the true value, off the tie
        s = from_d(u);
    }
    return (float)s;
}

float simd_fmaf_soft(float a, float b, float c) { return fmaf_one(a, b, c); }

// Four lanes at once on the fast path; any lane outside it, or on a tie, goes through fmaf_one.
typedef uint32_t vu32 __attribute__((vector_size(16), may_alias));
typedef int32_t vs32 __attribute__((vector_size(16), may_alias));

static inline vs32 sub_v(vf32 x) { vu32 u = (vu32)x; return ((u & 0x7f800000u) == 0) & ((u << 1) != 0); }

vf32 simd_fma4_soft(vf32 a, vf32 b, vf32 c) {
    if (__builtin_expect(__builtin_reduce_or(sub_v(a) | sub_v(b) | sub_v(c)) != 0, 0)) {
        vf32 r;
        for (int i = 0; i < 4; i++) r[i] = fmaf_one(a[i], b[i], c[i]);
        return r;
    }
    vf64x4 p = __builtin_convertvector(a, vf64x4) * __builtin_convertvector(b, vf64x4);
    vf64x4 s = p + __builtin_convertvector(c, vf64x4);
    vu64x4 u = (vu64x4)s;
    vs64x4 e = (vs64x4)(u >> 52 & 0x7ff);
    vs64x4 bad = (e <= F_LO) | (e > F_HI) | (vs64x4)((u & HALF_MASK) == HALF_BITS);
    if (__builtin_expect(__builtin_reduce_or(bad) != 0, 0)) {
        vf32 r;
        for (int i = 0; i < 4; i++) r[i] = fmaf_one(a[i], b[i], c[i]);
        return r;
    }
    return __builtin_convertvector(s, vf32);
}

// Double: Dekker's product gives a*b = p + q exactly (Veltkamp splits, no FMA needed) and a two-sum gives p + c = s + t
// exactly, so the fused result is the rounding of s + t + q. When t + q is itself exact (its two-sum error is zero),
// one rounded addition s + (t + q) is that rounding. That holds for most operands; otherwise, and outside the
// exponent range where the splits and error terms can neither overflow nor become subnormal, or under a directed
// rounding mode (the two-sums need round-to-nearest), the C library computes it.
#define D_LO (0x3ff - 900)
#define D_HI (0x3ff + 500)


static inline void split(double x, double *hi, double *lo) {
    double t = x * 134217729.0;   // 2^27 + 1
    *hi = t - (t - x);
    *lo = x - *hi;
}

static inline double fma_one(double a, double b, double c) {
    uint64_t ua = bits_d(a), ub = bits_d(b), uc = bits_d(c);
    int ea = (int)(ua >> 52 & 0x7ff), eb = (int)(ub >> 52 & 0x7ff), ec = (int)(uc >> 52 & 0x7ff);
    if ((ua << 1) == 0 || (ub << 1) == 0) return a * b + c;   // the product is an exact signed zero
    if (__builtin_expect(ea < D_LO || ea > D_HI || eb < D_LO || eb > D_HI || ea + eb - 0x3ff < D_LO ||
                         ea + eb - 0x3ff > D_HI || ((uc << 1) != 0 && (ec < D_LO || ec > D_HI)), 0))
        return fma_edge(a, b, c);
    if (__builtin_expect((_mm_getcsr() & 0x6000) != 0, 0)) return fma_edge(a, b, c);
    double p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    double q = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    double s = p + c, bb = s - p;
    double t = (p - (s - bb)) + (c - bb);
    double v = t + q, vb = v - t;
    double w = (t - (v - vb)) + (q - vb);
    double r = s + v;
    int er = (int)(bits_d(r) >> 52 & 0x7ff);
    if (__builtin_expect(w != 0 || ((bits_d(r) << 1) != 0 && er < D_LO), 0)) return fma_edge(a, b, c);
    return r;
}

double simd_fmad_soft(double a, double b, double c) { return fma_one(a, b, c); }

vf64 simd_fma2_soft(vf64 a, vf64 b, vf64 c) {
    vf64 r;
    for (int i = 0; i < 2; i++) r[i] = fma_one(a[i], b[i], c[i]);
    return r;
}

#endif
