// NV2A backend of the GX rasteriser hook (include/xbox/xrgpu.h), built on pbkit.
//
// The CPU side (xraster.cpp) transforms, clips and culls the GX primitives and hands this file finished triangles in window
// coordinates with the pixel state. Here they are drawn by the GPU:
//   * a pass-through vertex program (tools/xbox/shaders/gx.vs.cg compiled with nxdk's vp20compiler): position (window x, y,
//     z in 24 bit units, w = clip w), diffuse colour, four texture coordinate sets;
//   * the GX TEV stages become NV2A register combiners (general combiner = A*B + C*D, one TEV stage takes one or two of them);
//   * GX textures (decoded to ARGB by xraster.cpp) are uploaded as swizzled power-of-two A8R8G8B8 textures, cached by content;
//   * blend, depth test, alpha test, colour mask and scissor map to the matching NV097 registers.
// The EFB is pbkit's extra buffer 0. GXCopyDisp renders it into extra buffer 1 (the snapshot); the VI retrace draws the
// snapshot into the back buffer (centred), prints the HUD text and swaps. GXCopyTex reads the EFB back to the CPU.
//
// NOT verified on hardware or an emulator when written (the build machine has neither); the counters in the HUD tell what
// the GPU path did. Not done: lighting / texgen matrices (like the software path), TEV bias +0.5, 4x scale with bias, more than
// 8 combiner stages, more than 4 texture units per draw, lines and points, mipmaps, depth / alpha-only texture copies.
#ifdef NXDK
#include "xbox/xnv2a.h"

#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef MASK
#define MASK(mask, val) (((val) << (__builtin_ffs(mask) - 1)) & (mask))

namespace {

// ---- vertex program: out = in (see tools/xbox/shaders/gx.vs.cg) ----
const uint32_t VS_PROG[] = {
    0x00000000, 0x0020061b, 0x0836106c, 0x2070f818,
    0x00000000, 0x0020001b, 0x0836106c, 0x2070f800,
    0x00000000, 0x0020121b, 0x0836106c, 0x2070f848,
    0x00000000, 0x0020141b, 0x0836106c, 0x2070f850,
    0x00000000, 0x0020161b, 0x0836106c, 0x2070f858,
    0x00000000, 0x0020181b, 0x0836106c, 0x2070f861,
};

struct GV {   // the vertex as the program reads it: attribute 0 position, 3 diffuse, 9..12 texture coordinates
    float pos[4];
    float col[4];
    float uv[4][2];
};

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_V0 = 4, R_V1 = 5, R_T0 = 8, R_SP0 = 12, R_SP1 = 13 };
enum { M_ID = 6, M_INV = 1, M_NEG = 7 };

struct Stats {
    unsigned draws, tris, verts, texUp, texHit, texBytes, reads, copies, clears, presents;
    unsigned busyTO, finishTO, vblTO, ringWraps, arenaWraps, dwWraps;
    unsigned tevStages, nvStages, tevOver, biasSkip, constOver, unitOver, noTex, alphaApprox;
    unsigned lastDrawStages;
    unsigned efbNz, snapNz, efbCenter, snapCenter;
    unsigned copyNz, copyCenter, copyProbes, selfEfb, selfSnap, selfBack;
} g_s;

CRITICAL_SECTION g_cs;
bool g_on, g_haveCs;
uint8_t* g_ring;
unsigned g_ringSize, g_ringPos;
uint8_t* g_arena;
unsigned g_arenaSize, g_arenaPos;
unsigned g_dw;                 // dwords pushed since the last pb_reset
const unsigned DW_LIMIT = 440000;
int g_snapW, g_snapH;          // size of the picture in the snapshot buffer (0 = none yet)
int g_target = -1;             // 0 EFB, 1 snapshot, 2 back buffer
uint32_t g_whiteOff;           // 8x8 white texture
int g_W, g_H, g_pitch;

#define BEGIN() uint32_t* p = pb_begin(); uint32_t* p0 = p
#define END() do { g_dw += (unsigned) (p - p0); pb_end(p); } while (0)

inline uint32_t Phys(const void* ptr) { return (uint32_t) (uintptr_t) ptr & 0x03ffffffu; }

bool WaitIdle(unsigned ms)
{
    DWORD t0 = GetTickCount();
    while (pb_busy()) {
        if (GetTickCount() - t0 > ms) {
            g_s.busyTO++;
            return false;
        }
    }
    return true;
}

void SetControl0(void)
{
    BEGIN();
    p = pb_push1(p, NV097_SET_CONTROL0, 0x00100001);   // fixed point z, linear z, perspective correct textures, stencil write
    END();
}

void Target(int which)
{
    if (which == 0) pb_target_extra_buffer(0);
    else if (which == 1) pb_target_extra_buffer(1);
    else pb_target_back_buffer();
    g_target = which;
    SetControl0();
}

// ---- texture cache (swizzled A8R8G8B8, power-of-two) ----
struct TexEnt {
    unsigned id, ver;
    int w, h, pw, ph;
    uint32_t off;
    bool used;
} g_te[96];
int g_ten;

int Log2(int v)
{
    int l = 0;
    while ((1 << l) < v) l++;
    return l;
}

int Pow2(int v)
{
    int p2 = 1;
    while (p2 < v) p2 <<= 1;
    return p2;
}

void SwizzleMasks(int w, int h, uint32_t* mx, uint32_t* my)
{
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    for (;;) {
        bool any = false;
        if (bit < (uint32_t) w) { x |= mb; mb <<= 1; any = true; }
        if (bit < (uint32_t) h) { y |= mb; mb <<= 1; any = true; }
        bit <<= 1;
        if (!any) break;
    }
    *mx = x;
    *my = y;
}

uint32_t Deposit(uint32_t v, uint32_t mask)
{
    uint32_t r = 0;
    for (uint32_t bit = 1; mask; bit <<= 1) {
        uint32_t low = mask & (~mask + 1);
        if (v & bit) r |= low;
        mask &= mask - 1;
    }
    return r;
}

void ResetArena(void)
{
    g_arenaPos = 0;
    g_ten = 0;
    g_s.arenaWraps++;
}

// Uploads (or finds) a texture; returns false when it cannot be bound (the caller uses the white texel).
bool UploadTex(const XRGpuTex& t, uint32_t* off, int* pw, int* ph)
{
    if (!t.px || t.w <= 0 || t.h <= 0 || t.w > 1024 || t.h > 1024) return false;
    for (int i = 0; i < g_ten; i++) {
        const TexEnt& e = g_te[i];
        if (e.id == t.id && e.ver == t.ver && e.w == t.w && e.h == t.h) {
            *off = e.off; *pw = e.pw; *ph = e.ph;
            g_s.texHit++;
            return true;
        }
    }
    int w2 = Pow2(t.w), h2 = Pow2(t.h);
    unsigned bytes = (unsigned) w2 * (unsigned) h2 * 4u;
    if (g_arenaPos + bytes > g_arenaSize || g_ten >= (int) (sizeof(g_te) / sizeof(g_te[0]))) {
        WaitIdle(500);
        ResetArena();
        if (bytes > g_arenaSize) return false;
    }
    uint32_t mx, my;
    SwizzleMasks(w2, h2, &mx, &my);
    uint32_t* xo = (uint32_t*) malloc(sizeof(uint32_t) * (size_t) w2);
    uint32_t* yo = (uint32_t*) malloc(sizeof(uint32_t) * (size_t) h2);
    if (!xo || !yo) { free(xo); free(yo); return false; }
    for (int x = 0; x < w2; x++) xo[x] = Deposit((uint32_t) x, mx);
    for (int y = 0; y < h2; y++) yo[y] = Deposit((uint32_t) y, my);
    uint32_t* dst = (uint32_t*) (g_arena + g_arenaPos);
    for (int y = 0; y < h2; y++) {
        const unsigned* row = t.px + (size_t) (y < t.h ? y : t.h - 1) * (size_t) t.w;   // the padding repeats the last row / column
        for (int x = 0; x < w2; x++) dst[yo[y] + xo[x]] = row[x < t.w ? x : t.w - 1];
    }
    free(xo);
    free(yo);
    TexEnt& e = g_te[g_ten++];
    e.id = t.id; e.ver = t.ver; e.w = t.w; e.h = t.h; e.pw = w2; e.ph = h2;
    e.off = Phys(g_arena + g_arenaPos);
    e.used = true;
    g_arenaPos += (bytes + 127u) & ~127u;
    g_s.texUp++;
    g_s.texBytes += bytes;
    *off = e.off; *pw = w2; *ph = h2;
    return true;
}

int GxWrapToNv(int w)
{
    return w == 1 ? 1 : w == 2 ? 2 : 5;   // repeat, mirror, clamp to edge
}

void BindTexture(int unit, uint32_t off, int pw, int ph, int wrapS, int wrapT, bool bilinear, bool linearLayout, int pitch)
{
    BEGIN();
    int fmt = linearLayout ? NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 : NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    p = pb_push1(p, NV097_SET_TEXTURE_OFFSET + unit * 64, off);
    p = pb_push1(p, NV097_SET_TEXTURE_FORMAT + unit * 64,
                 MASK(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2) | MASK(NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR) |
                 MASK(NV097_SET_TEXTURE_FORMAT_COLOR, fmt) | MASK(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1) |
                 MASK(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2) |
                 MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, linearLayout ? 0 : Log2(pw)) |
                 MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, linearLayout ? 0 : Log2(ph)) | MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_P, 0));
    p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + unit * 64, (uint32_t) GxWrapToNv(wrapS) | ((uint32_t) GxWrapToNv(wrapT) << 8) | (1u << 16));
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + unit * 64, NV097_SET_TEXTURE_CONTROL0_ENABLE | MASK(NV097_SET_TEXTURE_CONTROL0_MIN_LOD_CLAMP, 0) |
                                                              MASK(NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP, 0));
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1 + unit * 64, MASK(NV097_SET_TEXTURE_CONTROL1_IMAGE_PITCH, pitch));
    p = pb_push1(p, NV097_SET_TEXTURE_FILTER + unit * 64, MASK(NV097_SET_TEXTURE_FILTER_MIN, bilinear ? 2 : 1) | MASK(NV097_SET_TEXTURE_FILTER_MAG, bilinear ? 2 : 1));
    p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT + unit * 64, ((uint32_t) pw << 16) | (uint32_t) ph);
    END();
}

void DisableTexture(int unit)
{
    BEGIN();
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + unit * 64, 0);
    END();
}

// ---- TEV -> register combiners ----
struct Inp {
    int reg, alpha, map;
    int kind;        // 0 register, 1 colour constant (k[0..2]), 2 alpha constant (k[3])
    float k[4];
};

struct Part {
    Inp in[4];
    int dst;         // register that receives A*B + C*D (0 = nothing)
    int op;          // output mapping (NV097_SET_COMBINER_COLOR_OCW_OP_*)
};

struct KSlot {
    float rgb[3], a;
    bool hasRgb, hasA;
};

struct NvStage {
    Part c, a;
    KSlot k[2];
};

Inp Plain(int reg, int alpha = 0, int map = M_ID)
{
    Inp i;
    memset(&i, 0, sizeof(i));
    i.reg = reg; i.alpha = alpha; i.map = map;
    return i;
}

Inp Zero(void) { return Plain(R_ZERO, 0, M_ID); }
Inp One(void) { return Plain(R_ZERO, 0, M_INV); }

Inp RgbConst(float r, float g, float b)
{
    Inp i = Plain(R_C0);
    i.kind = 1; i.k[0] = r; i.k[1] = g; i.k[2] = b;
    return i;
}

Inp AConst(float a, int replicate)
{
    Inp i = Plain(R_C0, replicate);
    i.kind = 2; i.k[3] = a;
    return i;
}

Inp Negated(Inp i) { i.map = (i.map == M_INV) ? M_INV : M_NEG; return i; }

Part NopPart(void)
{
    Part p;
    memset(&p, 0, sizeof(p));
    for (int i = 0; i < 4; i++) p.in[i] = Zero();
    return p;
}

struct TevCtx {
    const XRGpuBatch* b;
    int unitMap[4][2];       // (map, coord) -> texture unit
    int nunits;
    int regMap[4];           // TEV register -> NV2A register
    int scratch;
    bool cW[4], aW[4];       // written by an earlier stage (else the initial constant is read)
};

int UnitOf(TevCtx& t, int cmap, int ccoord)
{
    for (int i = 0; i < t.nunits; i++)
        if (t.unitMap[i][0] == cmap && t.unitMap[i][1] == ccoord) return i;
    if (t.nunits < 4) {
        t.unitMap[t.nunits][0] = cmap;
        t.unitMap[t.nunits][1] = ccoord;
        return t.nunits++;
    }
    g_s.unitOver++;
    return 3;
}

Inp ColorSel(TevCtx& t, const XRGpuStage& s, int sel)
{
    switch (sel) {
    case 0: case 2: case 4: case 6: case 1: case 3: case 5: case 7: {
        int r = sel >> 1, al = sel & 1;
        if (t.cW[r] && !al) return Plain(t.regMap[r], 0);
        if (t.aW[r] && al) return Plain(t.regMap[r], 1);
        const float* v = t.b->reg[r];
        if (al) return v[3] == 0 ? Zero() : AConst(v[3] / 255.0f, 1);
        return (v[0] == 0 && v[1] == 0 && v[2] == 0) ? Zero() : RgbConst(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f);
    }
    case 8: case 9:
        if (!s.needTex || s.cmap < 0) return Zero();
        return Plain(R_T0 + UnitOf(t, s.cmap, s.ccoord & 3), sel == 9);
    case 10: case 11:
        return s.cchan >= 0 ? Plain(R_V0, sel == 11) : Zero();
    case 12: return One();
    case 13: return RgbConst(0.5f, 0.5f, 0.5f);
    case 14: return RgbConst(s.kcv[0] / 255.0f, s.kcv[1] / 255.0f, s.kcv[2] / 255.0f);
    default: return Zero();
    }
}

Inp AlphaSel(TevCtx& t, const XRGpuStage& s, int sel)
{
    switch (sel) {
    case 0: case 1: case 2: case 3: {
        if (t.aW[sel]) return Plain(t.regMap[sel], 1);
        float v = t.b->reg[sel][3];
        return v == 0 ? Zero() : AConst(v / 255.0f, 1);
    }
    case 4:
        if (!s.needTex || s.cmap < 0) return Zero();
        return Plain(R_T0 + UnitOf(t, s.cmap, s.ccoord & 3), 1);
    case 5: return s.cchan >= 0 ? Plain(R_V0, 1) : Zero();
    case 6: return s.kav == 0 ? Zero() : AConst(s.kav / 255.0f, 1);
    default: return Zero();
    }
}

int OutputOp(int scale, int bias)
{
    // scale: GX 0 x1, 1 x2, 2 x4, 3 x0.5; bias: GX 0 none, 1 +0.5, 2 -0.5
    if (bias == 1) g_s.biasSkip++;
    bool neg = bias == 2;
    switch (scale) {
    case 0: return neg ? NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT_BIAS : NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT;
    case 1: return neg ? NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY1_BIAS : NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY1;
    case 2: if (neg) g_s.biasSkip++; return NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY2;
    default: if (neg) g_s.biasSkip++; return NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTRIGHTBY1;
    }
}

// (1-c)*a + c*b, then d +- that, for one channel group. Inputs already resolved; `zeroSel` are the GX selectors that mean 0 / 1.
int BuildChain(Inp a, Inp b, Inp c, Inp d, bool aZ, bool bZ, bool dZ, bool sub, int dst, int op, int scratch, Part* out)
{
    Inp onec = Inp(c);
    onec.map = (c.map == M_ID) ? M_INV : M_ID;   // 1 - c
    Inp bc = Inp(c);
    if (aZ && bZ) {
        out[0] = NopPart();
        out[0].in[0] = d; out[0].in[1] = One();
        out[0].dst = dst; out[0].op = op;
        return 1;
    }
    if (aZ) {          // d +- b*c
        out[0] = NopPart();
        out[0].in[0] = d; out[0].in[1] = One();
        out[0].in[2] = sub ? Negated(b) : b; out[0].in[3] = bc;
        out[0].dst = dst; out[0].op = op;
        return 1;
    }
    if (bZ) {          // d +- a*(1-c)
        out[0] = NopPart();
        out[0].in[0] = sub ? Negated(a) : a; out[0].in[1] = onec;
        out[0].in[2] = d; out[0].in[3] = One();
        out[0].dst = dst; out[0].op = op;
        return 1;
    }
    if (dZ) {          // +- ((1-c)*a + c*b)
        out[0] = NopPart();
        out[0].in[0] = sub ? Negated(a) : a; out[0].in[1] = onec;
        out[0].in[2] = sub ? Negated(b) : b; out[0].in[3] = bc;
        out[0].dst = dst; out[0].op = op;
        return 1;
    }
    out[0] = NopPart();   // lerp into a scratch register, then add d
    out[0].in[0] = a; out[0].in[1] = onec; out[0].in[2] = b; out[0].in[3] = bc;
    out[0].dst = scratch; out[0].op = NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT;
    out[1] = NopPart();
    out[1].in[0] = sub ? Negated(Plain(scratch)) : Plain(scratch); out[1].in[1] = One();
    out[1].in[2] = d; out[1].in[3] = One();
    out[1].dst = dst; out[1].op = op;
    return 2;
}

int NeedRgb(KSlot* k, const float* v)
{
    for (int i = 0; i < 2; i++)
        if (k[i].hasRgb && fabsf(k[i].rgb[0] - v[0]) < 1e-4f && fabsf(k[i].rgb[1] - v[1]) < 1e-4f && fabsf(k[i].rgb[2] - v[2]) < 1e-4f) return i;
    for (int i = 0; i < 2; i++)
        if (!k[i].hasRgb) { memcpy(k[i].rgb, v, sizeof(float) * 3); k[i].hasRgb = true; return i; }
    g_s.constOver++;
    return 0;
}

int NeedA(KSlot* k, float v)
{
    for (int i = 0; i < 2; i++)
        if (k[i].hasA && fabsf(k[i].a - v) < 1e-4f) return i;
    for (int i = 0; i < 2; i++)
        if (!k[i].hasA) { k[i].a = v; k[i].hasA = true; return i; }
    g_s.constOver++;
    return 0;
}

void ResolveConsts(NvStage* st, Part* pt)
{
    for (int i = 0; i < 4; i++) {
        Inp& in = pt->in[i];
        if (in.kind == 1) { in.reg = R_C0 + NeedRgb(st->k, in.k); in.kind = 0; }
        else if (in.kind == 2) { in.reg = R_C0 + NeedA(st->k, in.k[3]); in.kind = 0; }
    }
}

uint32_t Icw(const Part& pt, bool alphaChain)
{
    uint32_t v = 0;
    const Inp* in = pt.in;
    v |= MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, (uint32_t) in[0].reg) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, (uint32_t) (alphaChain || in[0].alpha)) | MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, (uint32_t) in[0].map);
    v |= MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, (uint32_t) in[1].reg) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, (uint32_t) (alphaChain || in[1].alpha)) | MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, (uint32_t) in[1].map);
    v |= MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, (uint32_t) in[2].reg) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, (uint32_t) (alphaChain || in[2].alpha)) | MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, (uint32_t) in[2].map);
    v |= MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, (uint32_t) in[3].reg) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, (uint32_t) (alphaChain || in[3].alpha)) | MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, (uint32_t) in[3].map);
    return v;
}

uint32_t Ocw(const Part& pt)
{
    return MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, (uint32_t) pt.dst) | MASK(NV097_SET_COMBINER_COLOR_OCW_OP, (uint32_t) pt.op);
}

uint32_t PackArgb(const KSlot& k)
{
    auto c8 = [](float v) -> uint32_t { int i = (int) (v * 255.0f + 0.5f); return (uint32_t) (i < 0 ? 0 : i > 255 ? 255 : i); };
    return (c8(k.a) << 24) | (c8(k.rgb[0]) << 16) | (c8(k.rgb[1]) << 8) | c8(k.rgb[2]);
}

// Builds the combiner chain for a batch; returns the number of NV2A stages and the texture units in use.
int BuildCombiners(const XRGpuBatch* b, NvStage* out, TevCtx* tc)
{
    memset(tc, 0, sizeof(*tc));
    tc->b = b;
    int n = 0;
    if (!b->tev || b->nstage <= 0) {
        out[0].c = NopPart(); out[0].a = NopPart();
        out[0].c.in[0] = Plain(R_V0); out[0].c.in[1] = One(); out[0].c.dst = R_SP0;
        out[0].a.in[0] = Plain(R_V0, 1); out[0].a.in[1] = One(); out[0].a.dst = R_SP0;
        memset(out[0].k, 0, sizeof(out[0].k));
        return 1;
    }
    // units first: every stage that samples fixes its (map, coord) pair, so register allocation knows which TEX regs are taken
    for (int i = 0; i < b->nstage && i < 16; i++) {
        const XRGpuStage& s = b->stage[i];
        if (s.needTex && s.cmap >= 0) UnitOf(*tc, s.cmap, s.ccoord & 3);
    }
    tc->regMap[0] = R_SP0;
    tc->regMap[1] = R_SP1;
    tc->regMap[2] = R_V1;
    tc->regMap[3] = R_V1;
    int freeReg[6], nf = 0;
    for (int u = 3; u >= tc->nunits; u--) freeReg[nf++] = R_T0 + u;
    if (nf > 0) tc->regMap[3] = freeReg[--nf];
    tc->scratch = nf > 0 ? freeReg[nf - 1] : R_SP1;
    for (int i = 0; i < b->nstage && i < 16; i++) {
        const XRGpuStage& s = b->stage[i];
        Part cp[2], ap[2];
        int cn, an;
        {
            Inp a = ColorSel(*tc, s, s.ca), bb = ColorSel(*tc, s, s.cb), c = ColorSel(*tc, s, s.cc), d = ColorSel(*tc, s, s.cd);
            bool aZ = s.ca == 15 || s.cc == 12, bZ = s.cb == 15 || s.cc == 15, dZ = s.cd == 15;
            cn = BuildChain(a, bb, c, d, aZ, bZ, dZ, s.cop != 0, tc->regMap[s.cdest & 3], OutputOp(s.cscale, s.cbias), tc->scratch, cp);
        }
        {
            Inp a = AlphaSel(*tc, s, s.aa), bb = AlphaSel(*tc, s, s.ab), c = AlphaSel(*tc, s, s.ac), d = AlphaSel(*tc, s, s.ad);
            bool aZ = s.aa == 7, bZ = s.ab == 7 || s.ac == 7, dZ = s.ad == 7;
            an = BuildChain(a, bb, c, d, aZ, bZ, dZ, s.aop != 0, tc->regMap[s.adest & 3], OutputOp(s.ascale, s.abias), tc->scratch, ap);
        }
        tc->cW[s.cdest & 3] = true;
        tc->aW[s.adest & 3] = true;
        int m = cn > an ? cn : an;
        g_s.tevStages++;
        if (n + m > 8) { g_s.tevOver++; break; }
        for (int j = 0; j < m; j++) {
            NvStage& st = out[n + j];
            memset(&st, 0, sizeof(st));
            st.c = (j >= m - cn) ? cp[j - (m - cn)] : NopPart();
            st.a = (j >= m - an) ? ap[j - (m - an)] : NopPart();
        }
        n += m;
    }
    if (n == 0) {
        out[0].c = NopPart(); out[0].a = NopPart();
        memset(out[0].k, 0, sizeof(out[0].k));
        n = 1;
    }
    for (int j = 0; j < n; j++) {
        ResolveConsts(&out[j], &out[j].c);
        ResolveConsts(&out[j], &out[j].a);
    }
    return n;
}

void EmitCombiners(const NvStage* st, int n)
{
    BEGIN();
    for (int i = 0; i < n; i++) {
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + i * 4, Icw(st[i].c, false));
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + i * 4, Ocw(st[i].c));
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + i * 4, Icw(st[i].a, true));
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + i * 4, Ocw(st[i].a));
        p = pb_push1(p, NV097_SET_COMBINER_FACTOR0 + i * 4, PackArgb(st[i].k[0]));
        p = pb_push1(p, NV097_SET_COMBINER_FACTOR1 + i * 4, PackArgb(st[i].k[1]));
    }
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL, MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_EACH_STAGE) |
                                                  MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_EACH_STAGE) |
                                                  MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, (uint32_t) n));
    // final combiner: out.rgb = spare0, out.a = spare0.a
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, R_SP0));
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, R_SP0) | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1));
    END();
}

void EmitStageProgram(int units)
{
    BEGIN();
    uint32_t s[4];
    for (int i = 0; i < 4; i++) s[i] = i < units ? NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE : 0;
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, s[0] | (s[1] << 5) | (s[2] << 10) | (s[3] << 15));
    END();
}

uint32_t BlendFactor(int f, bool isDst)
{
    switch (f) {
    case 0: return NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO;
    case 1: return NV097_SET_BLEND_FUNC_SFACTOR_V_ONE;
    case 2: return isDst ? NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR : NV097_SET_BLEND_FUNC_SFACTOR_V_DST_COLOR;
    case 3: return isDst ? NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_COLOR : NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_COLOR;
    case 4: return NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
    case 5: return NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_ALPHA;
    case 6: return NV097_SET_BLEND_FUNC_SFACTOR_V_DST_ALPHA;
    default: return NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_ALPHA;
    }
}

void EmitPixelState(const XRGpuBatch* b)
{
    BEGIN();
    unsigned bl = b->blend;
    bool blend = (bl & 1) != 0;
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, blend);
    if (blend) {
        int dstF = (int) ((bl >> 5) & 7), srcF = (int) ((bl >> 8) & 7);
        bool sub = ((bl >> 11) & 1) != 0;
        p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, BlendFactor(srcF, false));
        p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, BlendFactor(dstF, true));
        p = pb_push1(p, NV097_SET_BLEND_EQUATION, sub ? NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT : NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
    }
    p = pb_push1(p, NV097_SET_COLOR_MASK, (((bl >> 3) & 1) ? 0x00010101u : 0u) | (((bl >> 4) & 1) ? 0x01000000u : 0u));
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, b->zEnable ? 1 : 0);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, 0x200u + (uint32_t) (b->zFunc & 7));
    p = pb_push1(p, NV097_SET_DEPTH_MASK, (b->zEnable && b->zUpdate) ? 1 : 0);
    // alpha test: GX has two compares joined by a logic op, the NV2A one compare; the usual "x AND always" cases are exact
    bool at = b->aTest != 0;
    int func = 7, ref = 0;
    if (at) {
        if (b->acomp1 == 7 && b->alogic == 0) { func = b->acomp0; ref = b->aref0; }
        else if (b->acomp0 == 7 && b->alogic == 0) { func = b->acomp1; ref = b->aref1; }
        else { func = b->acomp0; ref = b->aref0; g_s.alphaApprox++; }
        if (func == 7) at = false;
    }
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, at ? 1 : 0);
    if (at) {
        p = pb_push1(p, NV097_SET_ALPHA_FUNC, 0x200u + (uint32_t) (func & 7));
        p = pb_push1(p, NV097_SET_ALPHA_REF, (uint32_t) ref & 255u);
    }
    // scissor
    uint32_t h = ((uint32_t) b->x1 << 16) | (uint32_t) b->x0, v = ((uint32_t) b->y1 << 16) | (uint32_t) b->y0;
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_TYPE, 0);
    pb_push(p++, NV097_SET_WINDOW_CLIP_HORIZONTAL, 8);
    for (int i = 0; i < 8; i++) *(p++) = h;
    pb_push(p++, NV097_SET_WINDOW_CLIP_VERTICAL, 8);
    for (int i = 0; i < 8; i++) *(p++) = v;
    END();
}

void SetArrays(uint32_t base)
{
    BEGIN();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 4) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(GV)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, base + 0);
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 4) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(GV)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, base + 16);
    for (int u = 0; u < 4; u++) {
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + (9 + u) * 4, MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 2) | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(GV)));
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + (9 + u) * 4, base + 32 + 8 * u);
    }
    END();
}

void DrawArrays(int first, int count)
{
    BEGIN();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
    while (count > 0) {
        int n = count > 252 ? 252 : count;
        p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS, MASK(NV097_DRAW_ARRAYS_COUNT, (uint32_t) (n - 1)) | MASK(NV097_DRAW_ARRAYS_START_INDEX, (uint32_t) first));
        first += n;
        count -= n;
    }
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    END();
}

// Makes room in the vertex ring / push buffer: waits for the GPU to be idle, then starts over.
void MakeRoom(unsigned verts)
{
    unsigned need = verts * (unsigned) sizeof(GV);
    if (g_ringPos + need > g_ringSize || g_dw > DW_LIMIT) {
        WaitIdle(500);
        if (g_ringPos + need > g_ringSize) { g_ringPos = 0; g_s.ringWraps++; }
        if (g_dw > DW_LIMIT) {
            pb_reset();
            g_dw = 0;
            g_s.dwWraps++;
            Target(g_target < 0 ? 0 : g_target);
        }
    }
}

void DrawBatch(const XRGpuBatch* b, const XRGpuVert* tri, int nverts)
{
    if (!g_on || nverts < 3) return;
    EnterCriticalSection(&g_cs);
    NvStage st[8];
    TevCtx tc;
    int ns = BuildCombiners(b, st, &tc);
    g_s.lastDrawStages = (unsigned) ns;
    g_s.nvStages += (unsigned) ns;
    // textures
    float scale[4][2];
    int coordOf[4] = {0, 0, 0, 0};
    for (int u = 0; u < 4; u++) { scale[u][0] = scale[u][1] = 1.0f; }
    for (int u = 0; u < tc.nunits; u++) {
        int map = tc.unitMap[u][0], coord = tc.unitMap[u][1];
        coordOf[u] = coord;
        const XRGpuTex& t = b->tex[map & 7];
        uint32_t off;
        int pw, ph;
        if (UploadTex(t, &off, &pw, &ph)) {
            BindTexture(u, off, pw, ph, t.wrapS, t.wrapT, t.linear != 0, false, pw * 4);
            scale[u][0] = (float) t.w / (float) pw;
            scale[u][1] = (float) t.h / (float) ph;
        } else {
            g_s.noTex++;
            BindTexture(u, g_whiteOff, 8, 8, 0, 0, false, false, 32);
        }
    }
    for (int u = tc.nunits; u < 4; u++) DisableTexture(u);
    EmitStageProgram(tc.nunits);
    EmitCombiners(st, ns);
    EmitPixelState(b);
    int done = 0;
    while (done < nverts) {
        int chunk = nverts - done;
        unsigned cap = g_ringSize / (unsigned) sizeof(GV);
        if ((unsigned) chunk > cap / 2) chunk = (int) (cap / 2);
        chunk -= chunk % 3;
        MakeRoom((unsigned) chunk);
        GV* o = (GV*) (g_ring + g_ringPos);
        for (int i = 0; i < chunk; i++) {
            const XRGpuVert& v = tri[done + i];
            o[i].pos[0] = v.x; o[i].pos[1] = v.y; o[i].pos[2] = v.z; o[i].pos[3] = v.w;
            for (int k = 0; k < 4; k++) o[i].col[k] = v.c[k];
            for (int u = 0; u < 4; u++) {
                o[i].uv[u][0] = v.tc[coordOf[u]][0] * scale[u][0];
                o[i].uv[u][1] = v.tc[coordOf[u]][1] * scale[u][1];
            }
        }
        SetArrays(Phys(g_ring + g_ringPos));
        DrawArrays(0, chunk);
        g_ringPos += (unsigned) chunk * (unsigned) sizeof(GV);
        done += chunk;
        g_s.verts += (unsigned) chunk;
        g_s.tris += (unsigned) chunk / 3;
    }
    g_s.draws++;
    LeaveCriticalSection(&g_cs);
}

// ---- copies of the whole screen: a textured rectangle from one of pbkit's buffers ----
void SimpleState(void)
{
    NvStage st[1];
    memset(st, 0, sizeof(st));
    st[0].c = NopPart(); st[0].a = NopPart();
    st[0].c.in[0] = Plain(R_T0); st[0].c.in[1] = One(); st[0].c.dst = R_SP0;
    st[0].a.in[0] = Plain(R_T0, 1); st[0].a.in[1] = One(); st[0].a.dst = R_SP0;
    EmitStageProgram(1);
    EmitCombiners(st, 1);
    XRGpuBatch b;
    memset(&b, 0, sizeof(b));
    b.x0 = 0; b.y0 = 0; b.x1 = g_W - 1; b.y1 = g_H - 1;
    b.zEnable = 0; b.zFunc = 7; b.zUpdate = 0;
    b.blend = (1u << 3) | (1u << 4);
    EmitPixelState(&b);
    for (int u = 1; u < 4; u++) DisableTexture(u);
}

// Draws the rectangle (sx, sy, w, h) of pbkit buffer `src` (extra buffer index) at (dx, dy) of the current target.
void CopyRect(int src, int sx, int sy, int w, int h, int dx, int dy)
{
    MakeRoom(6);
    {
        BEGIN();
        p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_ASK_FOR_IDLE, 0);   // the source was a render target a moment ago
        p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_NOP, 0);
        END();
    }
    SimpleState();
    BindTexture(0, Phys(pb_extra_buffer(src)), w, h, 0, 0, false, true, g_pitch);
    // linear textures take unnormalised coordinates
    GV* o = (GV*) (g_ring + g_ringPos);
    float x0 = (float) dx, y0 = (float) dy, x1 = (float) (dx + w), y1 = (float) (dy + h);
    float u0 = (float) sx, v0 = (float) sy, u1 = (float) (sx + w), v1 = (float) (sy + h);
    const float px[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    for (int i = 0; i < 6; i++) {
        memset(&o[i], 0, sizeof(GV));
        o[i].pos[0] = px[i][0]; o[i].pos[1] = px[i][1]; o[i].pos[2] = 0.0f; o[i].pos[3] = 1.0f;
        o[i].col[0] = o[i].col[1] = o[i].col[2] = o[i].col[3] = 1.0f;
        o[i].uv[0][0] = px[i][2]; o[i].uv[0][1] = px[i][3];
    }
    SetArrays(Phys(g_ring + g_ringPos));
    DrawArrays(0, 6);
    g_ringPos += 6u * (unsigned) sizeof(GV);
    g_s.copies++;
}

void ClearTarget(uint32_t flags, uint32_t argb, uint32_t z24)
{
    BEGIN();
    pb_push(p++, NV097_SET_CLEAR_RECT_HORIZONTAL, 2);
    *(p++) = ((uint32_t) (g_W - 1) << 16) | 0u;
    *(p++) = ((uint32_t) (g_H - 1) << 16) | 0u;
    pb_push(p++, NV097_SET_ZSTENCIL_CLEAR_VALUE, 3);
    *(p++) = z24 << 8;
    *(p++) = argb;
    *(p++) = flags;
    END();
}

unsigned CountNonBlack(int idx, unsigned* centre);
void TestTriangle(void);

// ---- XRGpu hooks ----
void HookDraw(const XRGpuBatch* b, const XRGpuVert* tri, int nverts) { DrawBatch(b, tri, nverts); }

void HookReadback(unsigned* dst, int stride, int x, int y, int w, int h)
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    WaitIdle(500);
    const uint32_t* src = (const uint32_t*) pb_extra_buffer(0);
    int pitch = g_pitch / 4;
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        unsigned* d = dst + (size_t) r * (size_t) stride;
        if (yy >= g_H || x >= g_W) { memset(d, 0, sizeof(unsigned) * (size_t) w); continue; }
        int n = w;
        if (x + n > g_W) n = g_W - x;
        memcpy(d, src + (size_t) yy * (size_t) pitch + x, sizeof(unsigned) * (size_t) n);
        if (n < w) memset(d + n, 0, sizeof(unsigned) * (size_t) (w - n));
    }
    g_s.reads++;
    LeaveCriticalSection(&g_cs);
}

void HookClear(int rgb, int alpha, unsigned argb, int depth, unsigned z24)
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    MakeRoom(0);
    if (g_target != 0) Target(0);
    uint32_t flags = (depth ? 0x03u : 0u) | (rgb ? 0x70u : 0u) | (alpha ? 0x80u : 0u);
    if (flags) ClearTarget(flags, argb, z24 & 0xFFFFFFu);
    g_s.clears++;
    LeaveCriticalSection(&g_cs);
}

void HookDisplay(int w, int h)
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    if (w > g_W) w = g_W;
    if (h > g_H) h = g_H;
    if ((g_s.copies & 15) == 0) {   // diagnostic: what is in the EFB at the moment the game copies it
        WaitIdle(500);
        g_s.copyNz = CountNonBlack(0, &g_s.copyCenter);
        g_s.copyProbes++;
    }
    Target(1);
    CopyRect(0, 0, 0, w, h, 0, 0);
    Target(0);
    g_snapW = w;
    g_snapH = h;
    LeaveCriticalSection(&g_cs);
}

XRGpu g_gpu = {HookDraw, HookReadback, HookClear, HookDisplay, 1};

void WaitVbl(void)
{
    DWORD v = pb_get_vbl_counter(), t0 = GetTickCount();
    while (pb_get_vbl_counter() == v) {
        if (GetTickCount() - t0 > 60) { g_s.vblTO++; break; }
        Sleep(1);
    }
}

// Diagnostics: how many of a sparse grid of pixels are not black in the EFB (draws reached it) and in the snapshot (GXCopyDisp worked).
unsigned CountNonBlack(int idx, unsigned* centre)
{
    const uint32_t* src = (const uint32_t*) pb_extra_buffer(idx);
    int pitch = g_pitch / 4, n = 0;
    for (int j = 0; j < 28; j++)
        for (int i = 0; i < 32; i++) {
            int x = 10 + i * 20, y = 8 + j * 16;
            if (x < g_W && y < g_H && (src[(size_t) y * (size_t) pitch + x] & 0xFFFFFFu)) n++;
        }
    *centre = src[(size_t) (g_H / 2) * (size_t) pitch + g_W / 2];
    return (unsigned) n;
}

// A gradient triangle drawn through the whole 3D path (vertex program + combiners): if it shows, the pipeline works.
void TestTriangle(void)
{
    XRGpuBatch b;
    memset(&b, 0, sizeof(b));
    b.x1 = g_W - 1; b.y1 = g_H - 1;
    b.zFunc = 7;
    b.blend = (1u << 3) | (1u << 4);
    XRGpuVert v[3];
    memset(v, 0, sizeof(v));
    const float xy[3][2] = {{560, 400}, {620, 400}, {560, 460}};
    const float col[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (int i = 0; i < 3; i++) {
        v[i].x = xy[i][0]; v[i].y = xy[i][1]; v[i].z = 0; v[i].w = 1;
        for (int k = 0; k < 4; k++) v[i].c[k] = col[i][k];
    }
    DrawBatch(&b, v, 3);
}

}  // namespace

extern "C" {

int XNv2a_Init(void)
{
    if (g_on) return 1;
    if (!g_haveCs) { InitializeCriticalSection(&g_cs); g_haveCs = true; }
    pb_extra_buffers(2);
    pb_size(2 * 1024 * 1024);
    if (pb_init() != 0) return 0;
    pb_show_front_screen();
    g_W = (int) pb_back_buffer_width();
    g_H = (int) pb_back_buffer_height();
    g_pitch = (int) pb_back_buffer_pitch();
    g_ringSize = 3u << 20;
    g_arenaSize = 8u << 20;
    g_ring = (uint8_t*) MmAllocateContiguousMemoryEx(g_ringSize, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    g_arena = (uint8_t*) MmAllocateContiguousMemoryEx(g_arenaSize + 4096, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!g_ring || !g_arena) {
        if (g_ring) MmFreeContiguousMemory(g_ring);
        if (g_arena) MmFreeContiguousMemory(g_arena);
        g_ring = g_arena = 0;
        pb_kill();
        return 0;
    }
    // 8x8 white texture at the end of the arena (never recycled)
    {
        uint32_t* w = (uint32_t*) (g_arena + g_arenaSize);
        for (int i = 0; i < 64; i++) w[i] = 0xFFFFFFFFu;
        g_whiteOff = Phys(w);
    }
    pb_reset();
    g_dw = 0;
    {   // vertex program
        BEGIN();
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                     MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                         MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
        END();
        {
            BEGIN();
            p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
            END();
        }
        for (unsigned i = 0; i < sizeof(VS_PROG) / 16; i++) {
            BEGIN();
            pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
            memcpy(p, &VS_PROG[i * 4], 16);
            p += 4;
            END();
        }
    }
    {   // all attributes off (the draws enable the ones they use), no culling in hardware (done on the CPU), no fog / lighting
        BEGIN();
        pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
        for (int i = 0; i < 16; i++) *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
        p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
        END();
    }
    Target(1);
    ClearTarget(0xF3u, 0xFF000000u, 0xFFFFFFu);
    Target(0);
    ClearTarget(0xF3u, 0xFF000000u, 0xFFFFFFu);
    g_on = true;
    {   // self test of the EFB -> snapshot copy: a triangle in the EFB, copied, both read back
        Target(0);
        ClearTarget(0xF3u, 0xFF000000u, 0xFFFFFFu);
        TestTriangle();
        Target(1);
        CopyRect(0, 0, 0, g_W, g_H, 0, 0);
        Target(0);
        WaitIdle(1000);
        const int px = 575, py = 405;   // inside the test triangle
        g_s.selfEfb = ((const uint32_t*) pb_extra_buffer(0))[(size_t) py * (size_t) (g_pitch / 4) + px];
        g_s.selfSnap = ((const uint32_t*) pb_extra_buffer(1))[(size_t) py * (size_t) (g_pitch / 4) + px];
        ClearTarget(0xF3u, 0xFF000000u, 0xFFFFFFu);
        g_ringPos = 0;
    }
    return 1;
}

int XNv2a_Active(void) { return g_on ? 1 : 0; }
const XRGpu* XNv2a_Gpu(void) { return &g_gpu; }

void XNv2a_Present(int black, const char* const* lines, int nlines)
{
    if (!g_on) return;
    EnterCriticalSection(&g_cs);
    MakeRoom(6);
    WaitIdle(500);
    g_s.efbNz = CountNonBlack(0, &g_s.efbCenter);
    g_s.snapNz = CountNonBlack(1, &g_s.snapCenter);
    Target(2);
    ClearTarget(0xF3u, 0xFF000000u, 0xFFFFFFu);
    if (!black && g_snapW > 0 && g_snapH > 0) {
        int dx = (g_W - g_snapW) / 2, dy = (g_H - g_snapH) / 2;
        CopyRect(1, 0, 0, g_snapW, g_snapH, dx < 0 ? 0 : dx, dy < 0 ? 0 : dy);
    }
    TestTriangle();
    pb_erase_text_screen();
    for (int i = 0; i < nlines; i++) pb_print("%s\n", lines[i]);
    pb_draw_text_screen();
    WaitIdle(500);
    DWORD t0 = GetTickCount();
    while (pb_finished()) {
        if (GetTickCount() - t0 > 200) { g_s.finishTO++; break; }
    }
    g_s.presents++;
    // next frame
    WaitVbl();
    pb_reset();
    g_dw = 0;
    g_ringPos = 0;
    Target(0);
    LeaveCriticalSection(&g_cs);
}

void XNv2a_StatsLine(int which, char* out, int n)
{
    switch (which) {
    case 0:
        snprintf(out, (size_t) n, "gpu dr%u tri%u v%u tex up%u hit%u %uk rb%u cp%u clr%u", g_s.draws, g_s.tris, g_s.verts, g_s.texUp, g_s.texHit,
                 g_s.texBytes >> 10, g_s.reads, g_s.copies, g_s.clears);
        break;
    case 2:
        snprintf(out, (size_t) n, "at-copy nz%u c%08x (%u)  efb nz%u  snap nz%u  self e%08x s%08x", g_s.copyNz, g_s.copyCenter, g_s.copyProbes,
                 g_s.efbNz, g_s.snapNz, g_s.selfEfb, g_s.selfSnap);
        break;
    default:
        snprintf(out, (size_t) n, "gpu stg%u/%u over%u bias%u k%u unit%u notex%u aprx%u TO b%u f%u v%u wr r%u a%u d%u", g_s.nvStages, g_s.tevStages,
                 g_s.tevOver, g_s.biasSkip, g_s.constOver, g_s.unitOver, g_s.noTex, g_s.alphaApprox, g_s.busyTO, g_s.finishTO, g_s.vblTO, g_s.ringWraps,
                 g_s.arenaWraps, g_s.dwWraps);
        break;
    }
}

}  // extern "C"
#endif
