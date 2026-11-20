// RE4 for the original Xbox: the PowerPC "paired single" operations the game's inline assembly uses.
//
// The Gekko has 32 floating point registers that can each hold two singles (ps0, ps1), instructions that
// work on both halves (ps_madd, ps_merge00 ...) and quantized loads/stores (psq_l / psq_st) driven by the
// GQR registers. The game's hand written blocks (sin/cos, skinning, texture/shape unpacking) are
// rewritten in C on top of the helpers below; each helper is one instruction, so a block translates line
// by line and can be compared against the original assembly.
//
// Semantics follow the Gekko user manual: multiply-adds are fused (one rounding, fmaf); quantized stores
// truncate toward zero and saturate; NaN stores as the type's maximum. Loads and stores use the host byte
// order: the disc and save data are byte-swapped once on load by the endianness layer, so the game's own
// buffers are native.
#ifndef XBOX_XPS_H
#define XBOX_XPS_H

#ifdef RE4_XBOX

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XPs {
    float p0, p1;
} XPs;

// Fused multiply-add a*b+c with one rounding (what Gekko's ps_madd does). nxdk's pdclib fmaf is a stub that
// aborts, so this is our own: a*b is exact in a double (24+24 bits), the sum is rounded to odd, and the final
// rounding to float is then correct (the double rounding cannot go wrong with round-to-odd). It needs the
// x87 precision at 53 bits, so on i386 the control word is set for the call (the CRT may leave 64 bits).
static inline float XFmaf(float a, float b, float c)
{
    double p = (double) a * (double) b;
    double dc = (double) c;
    double s, bb, err;
    uint64_t bits;
    float r;
#if defined(__i386__) || defined(XPS_X87_CW)
    unsigned short cw, cw53;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw53 = (unsigned short) ((cw & ~0x300u) | 0x200u);
    __asm__ volatile("fldcw %0" : : "m"(cw53));
#endif
    s = p + dc;
    bb = s - p;
    err = (p - (s - bb)) + (dc - bb);   // exact error of the sum (TwoSum)
    if (err != 0.0 && s - s == 0.0) {   // inexact and finite: make the last bit odd, toward the true value
        __builtin_memcpy(&bits, &s, 8);
        if (!(bits & 1)) {
            if ((err > 0.0) == (s > 0.0)) {
                bits++;
            } else {
                bits--;
            }
            __builtin_memcpy(&s, &bits, 8);
        }
    }
    r = (float) s;
#if defined(__i386__) || defined(XPS_X87_CW)
    __asm__ volatile("fldcw %0" : : "m"(cw));
#endif
    return r;
}

// GQR0..7, set by the game (mtspr 912..919). GQR0 is 0 (floats). The OS keeps them per thread on the GC;
// here they are global, which is enough while every task sets the same values (GQR2..5, see XPs_InitGQR) and
// GQR6 is set right before use (setupGQR6).
extern uint32_t g_xpsGQR[8];
void XPs_InitGQR(void);  // GQR2 = u8, GQR3 = u16, GQR4 = s8, GQR5 = s16, all scale 0 (what the game's tasks set up)
#define XPS_SET_GQR(n, v) (g_xpsGQR[n] = (uint32_t) (v))

// quantization types of the GQR fields
enum { XPS_F32 = 0, XPS_U8 = 4, XPS_U16 = 5, XPS_S8 = 6, XPS_S16 = 7 };

static inline XPs xps(float a, float b)
{
    XPs r = {a, b};
    return r;
}

// ---- arithmetic. d = op(a, c, b) follows the mnemonic order `ps_madd d, a, c, b` (a*c + b)
static inline XPs ps_mr(XPs a) { return a; }
static inline XPs ps_neg(XPs a) { return xps(-a.p0, -a.p1); }
static inline XPs ps_abs(XPs a) { return xps(__builtin_fabsf(a.p0), __builtin_fabsf(a.p1)); }
static inline XPs ps_add(XPs a, XPs b) { return xps(a.p0 + b.p0, a.p1 + b.p1); }
static inline XPs ps_sub(XPs a, XPs b) { return xps(a.p0 - b.p0, a.p1 - b.p1); }
static inline XPs ps_mul(XPs a, XPs c) { return xps(a.p0 * c.p0, a.p1 * c.p1); }
static inline XPs ps_muls0(XPs a, XPs c) { return xps(a.p0 * c.p0, a.p1 * c.p0); }
static inline XPs ps_muls1(XPs a, XPs c) { return xps(a.p0 * c.p1, a.p1 * c.p1); }
static inline XPs ps_madd(XPs a, XPs c, XPs b) { return xps(XFmaf(a.p0, c.p0, b.p0), XFmaf(a.p1, c.p1, b.p1)); }
static inline XPs ps_msub(XPs a, XPs c, XPs b) { return xps(XFmaf(a.p0, c.p0, -b.p0), XFmaf(a.p1, c.p1, -b.p1)); }
static inline XPs ps_nmadd(XPs a, XPs c, XPs b) { return ps_neg(ps_madd(a, c, b)); }
static inline XPs ps_nmsub(XPs a, XPs c, XPs b) { return ps_neg(ps_msub(a, c, b)); }
static inline XPs ps_madds0(XPs a, XPs c, XPs b) { return xps(XFmaf(a.p0, c.p0, b.p0), XFmaf(a.p1, c.p0, b.p1)); }
static inline XPs ps_madds1(XPs a, XPs c, XPs b) { return xps(XFmaf(a.p0, c.p1, b.p0), XFmaf(a.p1, c.p1, b.p1)); }
// ps_sum0 d,a,c,b = (a0 + b1, c1); ps_sum1 d,a,c,b = (c0, a0 + b1)
static inline XPs ps_sum0(XPs a, XPs c, XPs b) { return xps(a.p0 + b.p1, c.p1); }
static inline XPs ps_sum1(XPs a, XPs c, XPs b) { return xps(c.p0, a.p0 + b.p1); }
// ps_sel d,a,c,b: per half, a >= 0 ? c : b (NaN selects b)
static inline XPs ps_sel(XPs a, XPs c, XPs b) { return xps(a.p0 >= 0.0f ? c.p0 : b.p0, a.p1 >= 0.0f ? c.p1 : b.p1); }
static inline XPs ps_merge00(XPs a, XPs b) { return xps(a.p0, b.p0); }
static inline XPs ps_merge01(XPs a, XPs b) { return xps(a.p0, b.p1); }
static inline XPs ps_merge10(XPs a, XPs b) { return xps(a.p1, b.p0); }
static inline XPs ps_merge11(XPs a, XPs b) { return xps(a.p1, b.p1); }

// ---- quantized load / store. `w` is the instruction's W bit (1: one value, ps1 loads as 1.0), `gqr` its index.
static inline float xps_scale_pow2(int e) { return ldexpf(1.0f, e); }
static inline int xps_ld_scale(uint32_t g) { int s = (int) ((g >> 24) & 0x3F); return s >= 32 ? s - 64 : s; }
static inline int xps_st_scale(uint32_t g) { int s = (int) ((g >> 8) & 0x3F); return s >= 32 ? s - 64 : s; }

static inline unsigned xps_size(unsigned type)
{
    switch (type) {
        case XPS_U8: case XPS_S8: return 1;
        case XPS_U16: case XPS_S16: return 2;
        default: return 4;
    }
}

static inline float xps_ld_one(const void* p, unsigned type, int scale)
{
    float v;
    switch (type) {
        case XPS_U8: v = (float) *(const uint8_t*) p; break;
        case XPS_S8: v = (float) *(const int8_t*) p; break;
        case XPS_U16: { uint16_t t; __builtin_memcpy(&t, p, 2); v = (float) t; break; }
        case XPS_S16: { int16_t t; __builtin_memcpy(&t, p, 2); v = (float) t; break; }
        default: { float t; __builtin_memcpy(&t, p, 4); return t; }   // floats are not scaled
    }
    return v * xps_scale_pow2(-scale);
}

static inline void xps_st_one(float v, void* p, unsigned type, int scale)
{
    if (type < XPS_U8 || type > XPS_S16) {
        __builtin_memcpy(p, &v, 4);
        return;
    }
    {
        double lo, hi, x = (double) v * (double) xps_scale_pow2(scale);
        switch (type) {
            case XPS_U8: lo = 0; hi = 255; break;
            case XPS_S8: lo = -128; hi = 127; break;
            case XPS_U16: lo = 0; hi = 65535; break;
            default: lo = -32768; hi = 32767; break;
        }
        if (x != x) x = hi;                 // NaN stores as the maximum
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        {
            long i = (long) x;              // toward zero
            switch (type) {
                case XPS_U8: *(uint8_t*) p = (uint8_t) i; break;
                case XPS_S8: *(int8_t*) p = (int8_t) i; break;
                case XPS_U16: { uint16_t t = (uint16_t) i; __builtin_memcpy(p, &t, 2); break; }
                default: { int16_t t = (int16_t) i; __builtin_memcpy(p, &t, 2); break; }
            }
        }
    }
}

// psq_l d, off(p), w, gqr
static inline XPs psq_l(const void* p, int w, int gqr)
{
    uint32_t g = g_xpsGQR[gqr];
    unsigned type = (g >> 16) & 7, n = xps_size(type);
    int scale = xps_ld_scale(g);
    XPs r;
    r.p0 = xps_ld_one(p, type, scale);
    r.p1 = w ? 1.0f : xps_ld_one((const uint8_t*) p + n, type, scale);
    return r;
}

// psq_st s, off(p), w, gqr
static inline void psq_st(XPs s, void* p, int w, int gqr)
{
    uint32_t g = g_xpsGQR[gqr];
    unsigned type = g & 7, n = xps_size(type);
    int scale = xps_st_scale(g);
    xps_st_one(s.p0, p, type, scale);
    if (!w) {
        xps_st_one(s.p1, (uint8_t*) p + n, type, scale);
    }
}

#ifdef __cplusplus
}
#endif

#endif
#endif
