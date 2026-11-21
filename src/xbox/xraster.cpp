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
    float sx, sy, sz;       // screen position (sz in 24 bit depth units), filled by Project
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
    if (v->present & XGP_HAS_COL0) {
        for (int i = 0; i < 4; i++) o->col[i] = (float) v->col[0][i];
    } else {
        for (int i = 0; i < 4; i++) o->col[i] = 255.0f;
    }
}

void Project(V* v, const XGpState* st)
{
    const unsigned* vp = st->xf + 0x101A;
    float iw = 1.0f / v->cw;
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
    r.sx = r.sy = r.sz = 0;
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
};

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
            Plot(c, x, y, w0 * p0->sz + w1 * p1->sz + w2 * p2->sz, col);
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

XGpBackend g_be = {Draw, 0, 0, 0, 0};

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

extern "C" const unsigned* XRaster_Pixels(void) { return g_pix; }
extern "C" const float* XRaster_Depth(void) { return g_z; }
extern "C" unsigned long long XRaster_Triangles(void) { return g_tris; }
