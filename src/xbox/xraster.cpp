// Software rasterizer behind XGpBackend, see include/xbox/xraster.h.
#include "xbox/xraster.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

unsigned* g_pix;
float* g_z;
unsigned long long g_tris;

// A vertex after the position matrix and projection (clip space) with its interpolated attributes.
struct V {
    float cx, cy, cz, cw;   // clip coordinates
    float col[4];           // r g b a, 0..255
    float tc[4][2];         // texture coordinates 0..3 (as the vertex stream gives them; texgen matrices are not applied yet)
    float sx, sy, sz;       // screen position (sz in 24 bit depth units), filled by Project
    float iw;               // 1 / clip w, for perspective-correct interpolation
};

inline float F(unsigned u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void ToClip(const XGpVertex* v, const XGpState* st, V* o)
{
    const unsigned* xf = st->xf;
    int mi = (v->present & XGP_HAS_POSMTX) ? v->posMtxIdx : (int) (st->matIdxA & 63);
    const unsigned* m = xf + mi * 4;
    const unsigned* p = xf + 0x1020;
    float x = v->pos[0], y = v->pos[1], z = v->pos[2];
    float ex = F(m[0]) * x + F(m[1]) * y + F(m[2]) * z + F(m[3]);
    float ey = F(m[4]) * x + F(m[5]) * y + F(m[6]) * z + F(m[7]);
    float ez = F(m[8]) * x + F(m[9]) * y + F(m[10]) * z + F(m[11]);
    if (xf[0x1026]) {  // orthographic
        o->cx = F(p[0]) * ex + F(p[1]);
        o->cy = F(p[2]) * ey + F(p[3]);
        o->cz = F(p[4]) * ez + F(p[5]);
        o->cw = 1.0f;
    } else {           // perspective: w = -z of the eye space
        o->cx = F(p[0]) * ex + F(p[1]) * ez;
        o->cy = F(p[2]) * ey + F(p[3]) * ez;
        o->cz = F(p[4]) * ez + F(p[5]);
        o->cw = -ez;
    }
    // channel 0 colour: vertex colour or the XF material register (XF 0x100E / 0x1010 bit 0 = vertex source). Lighting
    // is not evaluated: a lit channel shows its material colour.
    unsigned mat = xf[0x100C];
    bool vcol = (v->present & XGP_HAS_COL0) != 0;
    for (int i = 0; i < 3; i++) {
        bool fromVtx = vcol && (xf[0x100E] & 1);
        o->col[i] = fromVtx ? (float) v->col[0][i] : (float) ((mat >> (24 - 8 * i)) & 255);
    }
    o->col[3] = (vcol && (xf[0x1010] & 1)) ? (float) v->col[0][3] : (float) (mat & 255);
    for (int t = 0; t < 4; t++) {
        o->tc[t][0] = (v->present & (XGP_HAS_TEX0 << t)) ? v->tex[t][0] : 0.0f;
        o->tc[t][1] = (v->present & (XGP_HAS_TEX0 << t)) ? v->tex[t][1] : 0.0f;
    }
}

void Project(V* v, const XGpState* st)
{
    const unsigned* vp = st->xf + 0x101A;
    float iw = 1.0f / v->cw;
    v->iw = iw;
    v->sx = F(vp[0]) * v->cx * iw + F(vp[3]) - 342.0f;
    v->sy = F(vp[1]) * v->cy * iw + F(vp[4]) - 342.0f;
    v->sz = v->cz * iw * F(vp[2]) + F(vp[5]);
}

V Lerp(const V& a, const V& b, float t)
{
    V r;
    r.cx = a.cx + (b.cx - a.cx) * t;
    r.cy = a.cy + (b.cy - a.cy) * t;
    r.cz = a.cz + (b.cz - a.cz) * t;
    r.cw = a.cw + (b.cw - a.cw) * t;
    for (int i = 0; i < 4; i++) r.col[i] = a.col[i] + (b.col[i] - a.col[i]) * t;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 2; i++) r.tc[k][i] = a.tc[k][i] + (b.tc[k][i] - a.tc[k][i]) * t;
    r.sx = r.sy = r.sz = 0;
    r.iw = 0;
    return r;
}

// Signed distance to clip plane k: w > 0, near (z >= -w), far (z <= 0). >= 0 is inside.
inline float Dist(const V& v, int k)
{
    return k == 0 ? v.cw - 1e-6f : k == 1 ? v.cz + v.cw : -v.cz;
}

// Sutherland-Hodgman against the three planes; returns the vertex count (0..9).
int ClipPoly(const V* in, int n, V* out)
{
    V a[10], b[10];
    int na = n, nb;
    memcpy(a, in, n * sizeof(V));
    for (int k = 0; k < 3; k++) {
        nb = 0;
        for (int i = 0; i < na; i++) {
            const V& cur = a[i];
            const V& nxt = a[(i + 1) % na];
            float dc = Dist(cur, k), dn = Dist(nxt, k);
            if (dc >= 0) b[nb++] = cur;
            if ((dc >= 0) != (dn >= 0)) b[nb++] = Lerp(cur, nxt, dc / (dc - dn));
        }
        if (nb == 0) return 0;
        memcpy(a, b, nb * sizeof(V));
        na = nb;
    }
    memcpy(out, a, na * sizeof(V));
    return na;
}

struct Ctx {
    const XGpState* st;
    int x0, y0, x1, y1;   // scissor, inclusive, clamped to the EFB
    int cull;             // 0 none, 1 back, 2 front, 3 all
    int zEnable, zFunc, zUpdate;
    unsigned blend;
    // texture / TEV state, decoded once per batch
    struct Map {
        const unsigned char* data;
        int fmt, w, h, wrapS, wrapT;
        bool linear;
    } map[8];
    struct Stage {
        int cmap, ccoord, cchan;            // texture map (or -1), tex coord, raster channel (-1 = zero)
        int ca, cb, cc, cd, cbias, cop, cclamp, cscale, cdest;
        int aa, ab, ac, ad, abias, aop, aclamp, ascale, adest;
        int kc, ka;                         // constant selections
    } stage[16];
    int nstage;
    bool tev;                               // false: the TEV was never programmed, draw the vertex colour
    float kreg[4][4];
    float reg[4][4];                        // prev, c0, c1, c2 initial values (r g b a)
    int acomp0, acomp1, alogic, aref0, aref1;
};

float Signed11(unsigned v)
{
    v &= 0x7FF;
    return (float) (v & 0x400 ? (int) v - 0x800 : (int) v);
}

void SetupTev(const XGpState* st, Ctx* c)
{
    unsigned memSize = 0;
    unsigned char* mem = XGp_MemoryBase(&memSize);
    for (int m = 0; m < 8; m++) {
        int i = m & 3;
        unsigned mode0 = st->bp[(m < 4 ? 0x80 : 0xA0) + i];
        unsigned img0 = st->bp[(m < 4 ? 0x88 : 0xA8) + i];
        unsigned img3 = st->bp[(m < 4 ? 0x94 : 0xB4) + i];
        Ctx::Map& mp = c->map[m];
        mp.w = (int) (img0 & 0x3FF) + 1;
        mp.h = (int) ((img0 >> 10) & 0x3FF) + 1;
        mp.fmt = (int) ((img0 >> 20) & 15);
        mp.wrapS = (int) (mode0 & 3);
        mp.wrapT = (int) ((mode0 >> 2) & 3);
        mp.linear = (mode0 >> 4) & 1;
        unsigned off = (img3 & 0x1FFFFFF) << 5;
        mp.data = (mem && off < memSize) ? mem + off : 0;
    }
    c->nstage = (int) ((st->bp[0x00] >> 10) & 15) + 1;
    bool any = false;
    for (int i = 0; i < c->nstage; i++) {
        unsigned ce = st->bp[0xC0 + 2 * i], ae = st->bp[0xC1 + 2 * i];
        unsigned tr = st->bp[0x28 + i / 2];
        unsigned ks = st->bp[0xF6 + i / 2];
        Ctx::Stage& s = c->stage[i];
        if (ce || ae) any = true;
        unsigned t = (i & 1) ? tr >> 12 : tr;
        s.cmap = ((t >> 6) & 1) ? (int) (t & 7) : -1;
        s.ccoord = (int) ((t >> 3) & 7);
        int ch = (int) ((t >> 7) & 7);
        s.cchan = ch == 0 || ch == 1 ? 0 : -1;
        s.cd = (int) (ce & 15); s.cc = (int) ((ce >> 4) & 15); s.cb = (int) ((ce >> 8) & 15); s.ca = (int) ((ce >> 12) & 15);
        s.cbias = (int) ((ce >> 16) & 3); s.cop = (int) ((ce >> 18) & 1); s.cclamp = (int) ((ce >> 19) & 1);
        s.cscale = (int) ((ce >> 20) & 3); s.cdest = (int) ((ce >> 22) & 3);
        s.ad = (int) ((ae >> 4) & 7); s.ac = (int) ((ae >> 7) & 7); s.ab = (int) ((ae >> 10) & 7); s.aa = (int) ((ae >> 13) & 7);
        s.abias = (int) ((ae >> 16) & 3); s.aop = (int) ((ae >> 18) & 1); s.aclamp = (int) ((ae >> 19) & 1);
        s.ascale = (int) ((ae >> 20) & 3); s.adest = (int) ((ae >> 22) & 3);
        s.kc = (int) ((ks >> ((i & 1) ? 14 : 4)) & 31);
        s.ka = (int) ((ks >> ((i & 1) ? 19 : 9)) & 31);
    }
    c->tev = any;
    for (int i = 0; i < 4; i++) {
        unsigned ra = st->bp[0xE0 + 2 * i], bg = st->bp[0xE1 + 2 * i];
        c->reg[i][0] = Signed11(ra); c->reg[i][3] = Signed11(ra >> 12);
        c->reg[i][2] = Signed11(bg); c->reg[i][1] = Signed11(bg >> 12);
        ra = st->bp[0xE8 + 2 * i]; bg = st->bp[0xE9 + 2 * i];
        c->kreg[i][0] = Signed11(ra); c->kreg[i][3] = Signed11(ra >> 12);
        c->kreg[i][2] = Signed11(bg); c->kreg[i][1] = Signed11(bg >> 12);
    }
    unsigned ac = st->bp[0xF3];
    c->aref0 = (int) (ac & 255); c->aref1 = (int) ((ac >> 8) & 255);
    c->acomp0 = (int) ((ac >> 16) & 7); c->acomp1 = (int) ((ac >> 19) & 7); c->alogic = (int) ((ac >> 22) & 3);
}

void Setup(const XGpState* st, Ctx* c)
{
    unsigned s0 = st->bp[0x20], s1 = st->bp[0x21];
    c->st = st;
    c->x0 = (int) ((s0 >> 12) & 0xFFF) - 342;
    c->y0 = (int) (s0 & 0xFFF) - 342;
    c->x1 = (int) ((s1 >> 12) & 0xFFF) - 342;
    c->y1 = (int) (s1 & 0xFFF) - 342;
    if (c->x0 < 0) c->x0 = 0;
    if (c->y0 < 0) c->y0 = 0;
    if (c->x1 > XRASTER_W - 1) c->x1 = XRASTER_W - 1;
    if (c->y1 > XRASTER_H - 1) c->y1 = XRASTER_H - 1;
    c->cull = (int) ((st->bp[0x00] >> 14) & 3);
    c->zEnable = st->bp[0x40] & 1;
    c->zFunc = (int) ((st->bp[0x40] >> 1) & 7);
    c->zUpdate = (int) ((st->bp[0x40] >> 4) & 1);
    c->blend = st->bp[0x41];
    SetupTev(st, c);
}

bool ZTest(int func, float z, float old)
{
    switch (func) {
    case 0: return false;
    case 1: return z < old;
    case 2: return z == old;
    case 3: return z <= old;
    case 4: return z > old;
    case 5: return z != old;
    case 6: return z >= old;
    default: return true;
    }
}

inline unsigned char Clamp8(float f)
{
    return f <= 0 ? 0 : f >= 255 ? 255 : (unsigned char) (f + 0.5f);
}

// Blend factor k (0 zero, 1 one, 2 colour of the other side, 3 its inverse, 4 src alpha, 5 inverse, 6 dst alpha,
// 7 inverse) for channel ch (0..2 rgb) with source `s` and destination `d` in 0..255.
float Factor(int k, const float* s, const float* d, int ch, bool isDst)
{
    switch (k) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (isDst ? s[ch] : d[ch]) / 255.0f;
    case 3: return 1.0f - (isDst ? s[ch] : d[ch]) / 255.0f;
    case 4: return s[3] / 255.0f;
    case 5: return 1.0f - s[3] / 255.0f;
    case 6: return d[3] / 255.0f;
    default: return 1.0f - d[3] / 255.0f;
    }
}

void Plot(const Ctx& c, int x, int y, float z, const float* col)
{
    int i = y * XRASTER_W + x;
    if (z < 0) z = 0;
    if (z > 16777215.0f) z = 16777215.0f;
    if (c.zEnable && !ZTest(c.zFunc, z, g_z[i])) return;
    unsigned old = g_pix[i];
    float d[4] = {(float) ((old >> 16) & 255), (float) ((old >> 8) & 255), (float) (old & 255), (float) (old >> 24)};
    float o[4] = {col[0], col[1], col[2], col[3]};
    if (c.blend & 1) {
        int dstF = (int) ((c.blend >> 5) & 7), srcF = (int) ((c.blend >> 8) & 7);
        bool sub = (c.blend >> 11) & 1;
        for (int ch = 0; ch < 3; ch++) {
            float s = col[ch] * Factor(srcF, col, d, ch, false);
            float t = d[ch] * Factor(dstF, col, d, ch, true);
            o[ch] = sub ? t - s : s + t;
        }
    }
    unsigned r = Clamp8(o[0]), g = Clamp8(o[1]), b = Clamp8(o[2]), a = Clamp8(o[3]);
    if (!((c.blend >> 3) & 1)) { r = (old >> 16) & 255; g = (old >> 8) & 255; b = old & 255; }
    if (!((c.blend >> 4) & 1)) a = old >> 24;
    g_pix[i] = (a << 24) | (r << 16) | (g << 8) | b;
    if (c.zUpdate) g_z[i] = z;
}


// ---- texture decoding (GameCube formats; the data stays in game memory, big-endian) ----

inline unsigned Be16(const unsigned char* p) { return ((unsigned) p[0] << 8) | p[1]; }

// One texel (level 0) as r g b a in 0..255. `data` is the image, `lim` the end of game memory (bounds).
void Texel(const Ctx::Map& m, int x, int y, const unsigned char* lim, float* o)
{
    int bw, bh, bytes = 32;
    switch (m.fmt) {
    case 0: case 8: bw = 8; bh = 8; break;           // I4, C4
    case 1: case 2: case 9: bw = 8; bh = 4; break;   // I8, IA4, C8
    case 0xE: bw = 8; bh = 8; break;                 // CMPR
    case 6: bw = 4; bh = 4; bytes = 64; break;       // RGBA8
    default: bw = 4; bh = 4; break;                  // IA8, RGB565, RGB5A3, C14X2
    }
    int bpr = (m.w + bw - 1) / bw;
    int bx = x / bw, by = y / bh, ix = x % bw, iy = y % bh;
    const unsigned char* b = m.data + ((size_t) by * bpr + bx) * bytes;
    if (b + bytes > lim) {
        o[0] = o[1] = o[2] = 128; o[3] = 255;
        return;
    }
    float r = 0, g = 0, bl = 0, a = 255;
    switch (m.fmt) {
    case 0: {   // I4
        int idx = iy * 8 + ix;
        int v = (b[idx / 2] >> ((idx & 1) ? 0 : 4)) & 15;
        r = g = bl = a = (float) (v * 17);
        break;
    }
    case 1: r = g = bl = a = (float) b[iy * 8 + ix]; break;   // I8
    case 2: {   // IA4
        int v = b[iy * 8 + ix];
        a = (float) ((v >> 4) * 17);
        r = g = bl = (float) ((v & 15) * 17);
        break;
    }
    case 3: {   // IA8
        const unsigned char* q = b + (iy * 4 + ix) * 2;
        a = q[0];
        r = g = bl = q[1];
        break;
    }
    case 4: {   // RGB565
        unsigned v = Be16(b + (iy * 4 + ix) * 2);
        r = (float) (((v >> 11) & 31) * 255 / 31);
        g = (float) (((v >> 5) & 63) * 255 / 63);
        bl = (float) ((v & 31) * 255 / 31);
        break;
    }
    case 5: {   // RGB5A3
        unsigned v = Be16(b + (iy * 4 + ix) * 2);
        if (v & 0x8000) {
            r = (float) (((v >> 10) & 31) * 255 / 31);
            g = (float) (((v >> 5) & 31) * 255 / 31);
            bl = (float) ((v & 31) * 255 / 31);
        } else {
            a = (float) (((v >> 12) & 7) * 255 / 7);
            r = (float) (((v >> 8) & 15) * 17);
            g = (float) (((v >> 4) & 15) * 17);
            bl = (float) ((v & 15) * 17);
        }
        break;
    }
    case 6: {   // RGBA8: 32 bytes of AR then 32 bytes of GB
        int k = (iy * 4 + ix) * 2;
        a = b[k]; r = b[k + 1]; g = b[32 + k]; bl = b[32 + k + 1];
        break;
    }
    case 8: case 9: {   // CI4 / CI8: the palette is not loaded yet, shown as grey
        int v = m.fmt == 8 ? (b[(iy * 8 + ix) / 2] >> (((iy * 8 + ix) & 1) ? 0 : 4)) & 15 : b[iy * 8 + ix];
        r = g = bl = (float) (m.fmt == 8 ? v * 17 : v);
        break;
    }
    case 0xE: {   // CMPR: four 4x4 DXT1 blocks, order (0,0) (4,0) (0,4) (4,4)
        const unsigned char* q = b + ((iy / 4) * 2 + (ix / 4)) * 8;
        unsigned c0 = Be16(q), c1 = Be16(q + 2);
        float cc[4][4];
        unsigned cs[2] = {c0, c1};
        for (int k = 0; k < 2; k++) {
            cc[k][0] = (float) (((cs[k] >> 11) & 31) * 255 / 31);
            cc[k][1] = (float) (((cs[k] >> 5) & 63) * 255 / 63);
            cc[k][2] = (float) ((cs[k] & 31) * 255 / 31);
            cc[k][3] = 255;
        }
        for (int k = 0; k < 3; k++) {
            if (c0 > c1) {
                cc[2][k] = (2 * cc[0][k] + cc[1][k]) / 3;
                cc[3][k] = (cc[0][k] + 2 * cc[1][k]) / 3;
            } else {
                cc[2][k] = (cc[0][k] + cc[1][k]) / 2;
                cc[3][k] = 0;
            }
        }
        cc[2][3] = 255;
        cc[3][3] = c0 > c1 ? 255 : 0;
        int sel = (q[4 + (iy & 3)] >> (6 - 2 * (ix & 3))) & 3;
        r = cc[sel][0]; g = cc[sel][1]; bl = cc[sel][2]; a = cc[sel][3];
        break;
    }
    default: r = g = bl = 255; break;
    }
    o[0] = r; o[1] = g; o[2] = bl; o[3] = a;
}

inline int Wrap(int v, int n, int mode)
{
    if (mode == 1) {   // repeat
        v %= n;
        return v < 0 ? v + n : v;
    }
    if (mode == 2) {   // mirror
        int p = 2 * n;
        v %= p;
        if (v < 0) v += p;
        return v < n ? v : p - 1 - v;
    }
    return v < 0 ? 0 : v >= n ? n - 1 : v;   // clamp
}

// Samples map `m` at texture coordinates (s, t) in texels (the vertex stream gives them in texels already scaled
// by the VAT; GX texture coordinates are normalised, so they are multiplied by the size here).
void Sample(const Ctx::Map& m, float s, float t, float* o)
{
    unsigned memSize = 0;
    unsigned char* mem = XGp_MemoryBase(&memSize);
    if (!m.data || !mem || m.w <= 0 || m.h <= 0) {
        o[0] = o[1] = o[2] = o[3] = 255;
        return;
    }
    const unsigned char* lim = mem + memSize;
    float u = s * (float) m.w, v = t * (float) m.h;
    if (!m.linear) {
        Texel(m, Wrap((int) floorf(u), m.w, m.wrapS), Wrap((int) floorf(v), m.h, m.wrapT), lim, o);
        return;
    }
    u -= 0.5f; v -= 0.5f;
    int x0 = (int) floorf(u), y0 = (int) floorf(v);
    float fx = u - (float) x0, fy = v - (float) y0;
    float a[4], b[4], c[4], d[4];
    Texel(m, Wrap(x0, m.w, m.wrapS), Wrap(y0, m.h, m.wrapT), lim, a);
    Texel(m, Wrap(x0 + 1, m.w, m.wrapS), Wrap(y0, m.h, m.wrapT), lim, b);
    Texel(m, Wrap(x0, m.w, m.wrapS), Wrap(y0 + 1, m.h, m.wrapT), lim, c);
    Texel(m, Wrap(x0 + 1, m.w, m.wrapS), Wrap(y0 + 1, m.h, m.wrapT), lim, d);
    for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

// ---- TEV ----

float Konst(const Ctx& c, int sel, bool alpha)
{
    static const float frac[8] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
    if (sel < 8) return frac[sel] * 255.0f;
    if (sel >= 12 && !alpha && sel < 16) return 255.0f;   // colour constants are handled by the caller
    int k = sel & 3, comp = alpha ? (sel - 16) >> 2 : (sel - 16) >> 2;   // 16..19 R, 20..23 G, 24..27 B, 28..31 A
    if (sel < 16) return 255.0f;
    static const int idx[4] = {0, 1, 2, 3};
    return c.kreg[k][idx[comp & 3]];
}

float Clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Runs the TEV stages for one pixel; col = rasterised colour, tc = interpolated texture coordinates.
void Tev(const Ctx& c, const float* col, const float (*tc)[2], float* out)
{
    float reg[4][4];
    memcpy(reg, c.reg, sizeof(reg));
    for (int i = 0; i < c.nstage; i++) {
        const Ctx::Stage& s = c.stage[i];
        float tex[4] = {255, 255, 255, 255}, ras[4] = {0, 0, 0, 0};
        if (s.cmap >= 0) Sample(c.map[s.cmap], tc[s.ccoord & 3][0], tc[s.ccoord & 3][1], tex);
        if (s.cchan >= 0) memcpy(ras, col, sizeof(ras));
        float kc[3], ka;
        if (s.kc >= 16) {
            int k = (s.kc - 16) & 3, comp = (s.kc - 16) >> 2;   // R G B A of K0..K3 (as grey)
            kc[0] = kc[1] = kc[2] = c.kreg[k][comp & 3];
        } else if (s.kc >= 12) {
            int k = s.kc - 12;
            kc[0] = c.kreg[k][0]; kc[1] = c.kreg[k][1]; kc[2] = c.kreg[k][2];
        } else {
            kc[0] = kc[1] = kc[2] = Konst(c, s.kc, false);
        }
        ka = Konst(c, s.ka, true);
        float in[16][3];
        for (int k = 0; k < 3; k++) {
            in[0][k] = reg[0][k]; in[1][k] = reg[0][3];
            in[2][k] = reg[1][k]; in[3][k] = reg[1][3];
            in[4][k] = reg[2][k]; in[5][k] = reg[2][3];
            in[6][k] = reg[3][k]; in[7][k] = reg[3][3];
            in[8][k] = tex[k]; in[9][k] = tex[3];
            in[10][k] = ras[k]; in[11][k] = ras[3];
            in[12][k] = 255; in[13][k] = 128; in[14][k] = kc[k]; in[15][k] = 0;
        }
        float ain[8] = {reg[0][3], reg[1][3], reg[2][3], reg[3][3], tex[3], ras[3], ka, 0};
        static const float bias[4] = {0, 128, -128, 0};
        static const float scale[4] = {1, 2, 4, 0.5f};
        float res[4];
        for (int k = 0; k < 3; k++) {
            float a = in[s.ca][k], b = in[s.cb][k], cc = in[s.cc][k] / 255.0f, d = in[s.cd][k];
            float v = (1 - cc) * a + cc * b;
            v = (s.cop ? d - v : d + v) + bias[s.cbias];
            v *= scale[s.cscale];
            res[k] = s.cclamp ? Clampf(v, 0, 255) : Clampf(v, -1024, 1023);
        }
        {
            float a = ain[s.aa], b = ain[s.ab], cc = ain[s.ac] / 255.0f, d = ain[s.ad];
            float v = (1 - cc) * a + cc * b;
            v = (s.aop ? d - v : d + v) + bias[s.abias];
            v *= scale[s.ascale];
            res[3] = s.aclamp ? Clampf(v, 0, 255) : Clampf(v, -1024, 1023);
        }
        for (int k = 0; k < 3; k++) reg[s.cdest][k] = res[k];
        reg[s.adest][3] = res[3];
    }
    for (int k = 0; k < 4; k++) out[k] = Clampf(reg[0][k], 0, 255);
}

bool ACmp(int op, int a, int ref)
{
    switch (op) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;
    }
}

bool AlphaPass(const Ctx& c, float alpha)
{
    int a = (int) (alpha + 0.5f);
    bool r0 = ACmp(c.acomp0, a, c.aref0), r1 = ACmp(c.acomp1, a, c.aref1);
    switch (c.alogic) {
    case 0: return r0 && r1;
    case 1: return r0 || r1;
    case 2: return r0 != r1;
    default: return r0 == r1;
    }
}

void FillTri(const Ctx& c, const V& a, const V& b, const V& d)
{
    float area = (b.sx - a.sx) * (d.sy - a.sy) - (d.sx - a.sx) * (b.sy - a.sy);
    if (area == 0 || !(fabsf(area) < 1e12f)) return;
    // y grows downwards on the screen, so a counter-clockwise (front) triangle has a negative area
    bool front = area < 0;
    if (c.cull == 3 || (c.cull == 1 && !front) || (c.cull == 2 && front)) return;
    const V *p0 = &a, *p1 = &b, *p2 = &d;
    if (area < 0) {  // make the area positive so the inside test is "all edge functions >= 0"
        p1 = &d;
        p2 = &b;
        area = -area;
    }
    float minx = fminf(p0->sx, fminf(p1->sx, p2->sx)), maxx = fmaxf(p0->sx, fmaxf(p1->sx, p2->sx));
    float miny = fminf(p0->sy, fminf(p1->sy, p2->sy)), maxy = fmaxf(p0->sy, fmaxf(p1->sy, p2->sy));
    int ix0 = (int) fmaxf((float) c.x0, floorf(minx)), ix1 = (int) fminf((float) c.x1, ceilf(maxx));
    int iy0 = (int) fmaxf((float) c.y0, floorf(miny)), iy1 = (int) fminf((float) c.y1, ceilf(maxy));
    float inv = 1.0f / area;
    g_tris++;
    for (int y = iy0; y <= iy1; y++) {
        for (int x = ix0; x <= ix1; x++) {
            float px = (float) x + 0.5f, py = (float) y + 0.5f;
            float w0 = (p2->sx - p1->sx) * (py - p1->sy) - (px - p1->sx) * (p2->sy - p1->sy);
            float w1 = (p0->sx - p2->sx) * (py - p2->sy) - (px - p2->sx) * (p0->sy - p2->sy);
            float w2 = (p1->sx - p0->sx) * (py - p0->sy) - (px - p0->sx) * (p1->sy - p0->sy);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            w0 *= inv;
            w1 *= inv;
            w2 *= inv;
            float col[4];
            for (int k = 0; k < 4; k++) col[k] = w0 * p0->col[k] + w1 * p1->col[k] + w2 * p2->col[k];
            float z = w0 * p0->sz + w1 * p1->sz + w2 * p2->sz;
            if (c.tev) {   // perspective-correct texture coordinates, then the TEV
                float q0 = w0 * p0->iw, q1 = w1 * p1->iw, q2 = w2 * p2->iw, qs = q0 + q1 + q2;
                float tc[4][2];
                if (qs != 0) {
                    q0 /= qs; q1 /= qs; q2 /= qs;
                } else {
                    q0 = w0; q1 = w1; q2 = w2;
                }
                for (int t = 0; t < 4; t++)
                    for (int k = 0; k < 2; k++) tc[t][k] = q0 * p0->tc[t][k] + q1 * p1->tc[t][k] + q2 * p2->tc[t][k];
                float out[4];
                Tev(c, col, tc, out);
                if (!AlphaPass(c, out[3])) continue;
                Plot(c, x, y, z, out);
            } else {
                if (!AlphaPass(c, col[3])) continue;
                Plot(c, x, y, z, col);
            }
        }
    }
}

void Tri(const Ctx& c, const V& a, const V& b, const V& d)
{
    V in[3] = {a, b, d}, poly[10];
    int n = ClipPoly(in, 3, poly);
    if (n < 3) return;
    for (int i = 0; i < n; i++) Project(&poly[i], c.st);
    for (int i = 1; i + 1 < n; i++) FillTri(c, poly[0], poly[i], poly[i + 1]);
}

void Line(const Ctx& c, V a, V b)
{
    if (a.cw <= 1e-6f || b.cw <= 1e-6f) return;
    Project(&a, c.st);
    Project(&b, c.st);
    float dx = b.sx - a.sx, dy = b.sy - a.sy;
    int n = (int) fmaxf(fabsf(dx), fabsf(dy));
    if (n > 4096) return;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float) i / (float) n : 0.0f;
        int x = (int) floorf(a.sx + dx * t), y = (int) floorf(a.sy + dy * t);
        if (x < c.x0 || x > c.x1 || y < c.y0 || y > c.y1) continue;
        Plot(c, x, y, a.sz + (b.sz - a.sz) * t, a.col);
    }
}

void Draw(int prim, int, const XGpVertex* v, int count, const XGpState* st, void*)
{
    if (!g_pix || count <= 0) return;
    Ctx c;
    Setup(st, &c);
    V* t = (V*) malloc(sizeof(V) * (size_t) count);
    if (!t) return;
    for (int i = 0; i < count; i++) ToClip(&v[i], st, &t[i]);
    switch (prim) {
    case XGP_QUADS:
        for (int i = 0; i + 3 < count; i += 4) {
            Tri(c, t[i], t[i + 1], t[i + 2]);
            Tri(c, t[i], t[i + 2], t[i + 3]);
        }
        break;
    case XGP_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) Tri(c, t[i], t[i + 1], t[i + 2]);
        break;
    case XGP_TRISTRIP:
        for (int i = 0; i + 2 < count; i++) {
            if (i & 1) Tri(c, t[i + 1], t[i], t[i + 2]);
            else Tri(c, t[i], t[i + 1], t[i + 2]);
        }
        break;
    case XGP_TRIFAN:
        for (int i = 1; i + 1 < count; i++) Tri(c, t[0], t[i], t[i + 1]);
        break;
    case XGP_LINES:
        for (int i = 0; i + 1 < count; i += 2) Line(c, t[i], t[i + 1]);
        break;
    case XGP_LINESTRIP:
        for (int i = 0; i + 1 < count; i++) Line(c, t[i], t[i + 1]);
        break;
    case XGP_POINTS:
        for (int i = 0; i < count; i++) Line(c, t[i], t[i]);
        break;
    }
    free(t);
}

unsigned g_copySerial;

inline void Yuv(unsigned argb, int* y, int* u, int* v)
{
    int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    *y = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
    *u = (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
    *v = (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
}

// BP 0x52 (copy command): bit 14 = copy to an XFB (else a texture, not done yet), bit 11 = clear the EFB afterwards,
// bit 10 = vertical scale (BP 0x4E). The XFB is YUV422 as the video encoder reads it: Y0 U Y1 V per pixel pair.
void CopyCmd(unsigned cmd, const XGpState* st)
{
    if (!g_pix) return;
    if ((cmd >> 14) & 1) {
        unsigned memSize = 0;
        unsigned char* mem = XGp_MemoryBase(&memSize);
        unsigned src = st->bp[0x49], size = st->bp[0x4A];
        int x0 = (int) (src & 0x3FF), y0 = (int) ((src >> 10) & 0x3FF);
        int w = (int) (size & 0x3FF) + 1, h = (int) ((size >> 10) & 0x3FF) + 1;
        unsigned stride = (st->bp[0x4D] & 0x3FF) * 32u;
        unsigned dst = (st->bp[0x4B] & 0x1FFFFF) << 5;
        unsigned sc = st->bp[0x4E] & 0x1FF;
        int rows = h;
        if (((cmd >> 10) & 1) && sc) {  // the line count the SDK computes for this scale (__GXGetNumXfbLines)
            unsigned d = sc, n = ((unsigned) h - 1) * 0x100 / sc + 1;
            if (d > 0x80 && d < 0x100) {
                while (d % 2 == 0) d /= 2;
                if ((unsigned) h % d == 0) n++;
            }
            rows = (int) (n > 0x400 ? 0x400 : n);
        }
        if (mem && stride && x0 + w <= XRASTER_W && y0 + h <= XRASTER_H &&
            (unsigned long long) dst + (unsigned long long) rows * stride <= memSize && stride >= (unsigned) w * 2) {
            for (int r = 0; r < rows; r++) {
                int sy = y0 + (((cmd >> 10) & 1) && sc ? (int) ((unsigned) r * sc / 256u) : r);
                const unsigned* line = g_pix + sy * XRASTER_W + x0;
                unsigned char* o = mem + dst + (unsigned long long) r * stride;
                for (int x = 0; x < w; x += 2) {
                    int y1, u1, v1, y2, u2, v2;
                    Yuv(line[x], &y1, &u1, &v1);
                    Yuv(line[x + 1 < w ? x + 1 : x], &y2, &u2, &v2);
                    o[0] = (unsigned char) y1;
                    o[1] = (unsigned char) ((u1 + u2) / 2);
                    o[2] = (unsigned char) y2;
                    o[3] = (unsigned char) ((v1 + v2) / 2);
                    o += 4;
                }
            }
            g_copySerial++;
        }
    }
    if ((cmd >> 11) & 1) {
        unsigned c0 = st->bp[0x4F], c1 = st->bp[0x50], z = st->bp[0x51] & 0xFFFFFF;
        unsigned argb = ((c0 >> 8) & 255) << 24 | (c0 & 255) << 16 | ((c1 >> 8) & 255) << 8 | (c1 & 255);
        // the SDK widens the colour / depth masks of the clear through BP 0x40 / 0x41 first; honour them
        bool clrColor = (st->bp[0x41] >> 3) & 1, clrAlpha = (st->bp[0x41] >> 4) & 1, clrZ = (st->bp[0x40] >> 4) & 1;
        for (int i = 0; i < XRASTER_W * XRASTER_H; i++) {
            unsigned o = g_pix[i];
            unsigned n = argb;
            if (!clrColor) n = (n & 0xFF000000u) | (o & 0x00FFFFFFu);
            if (!clrAlpha) n = (n & 0x00FFFFFFu) | (o & 0xFF000000u);
            g_pix[i] = n;
            if (clrZ) g_z[i] = (float) z;
        }
    }
}

void OnBp(int reg, unsigned val, const XGpState* st, void*)
{
    if (reg == 0x52) CopyCmd(val, st);
}

XGpBackend g_be = {Draw, OnBp, 0, 0, 0};

}  // namespace

extern "C" void XRaster_Init(void)
{
    if (!g_pix) {
        g_pix = (unsigned*) malloc(sizeof(unsigned) * XRASTER_W * XRASTER_H);
        g_z = (float*) malloc(sizeof(float) * XRASTER_W * XRASTER_H);
    }
    g_tris = 0;
    XRaster_Clear(0xFF000000u, 16777215.0f);
}

extern "C" const XGpBackend* XRaster_Backend(void) { return &g_be; }

extern "C" void XRaster_Clear(unsigned argb, float z24)
{
    if (!g_pix) return;
    for (int i = 0; i < XRASTER_W * XRASTER_H; i++) {
        g_pix[i] = argb;
        g_z[i] = z24;
    }
}

extern "C" unsigned XRaster_CopySerial(void) { return g_copySerial; }
extern "C" const unsigned* XRaster_Pixels(void) { return g_pix; }
extern "C" const float* XRaster_Depth(void) { return g_z; }
extern "C" unsigned long long XRaster_Triangles(void) { return g_tris; }
