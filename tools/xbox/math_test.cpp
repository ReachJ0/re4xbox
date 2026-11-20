// Host test for the math layer: include/xbox/xps.h (the paired-single instructions), the game's inline assembly
// rewritten with it (math_sub.cpp SINF/COSF/LIMIT_ANGLE/SQRTF, trans.cpp CalcSk1_x/x2, PSMTXReorder) and the PS->C
// wrappers. The game functions are cut out of the real sources by run_math_test.sh, so what runs here is what ships.
#include <initializer_list>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox/xps.h"

typedef float f32;
typedef unsigned int u32;
#define PI 3.1415927f
#define PI2 6.2831855f

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool same(float a, float b) { return memcmp(&a, &b, 4) == 0 || (a == b); }

// ---- the game code under test
extern f32 Coeff[10], powx[2], sum[2];
extern "C" void* XMem_LockedCache(void);
f32 SQRTF(f32 x);
f32 SINF(f32 x);
f32 COSF(f32 x);
f32 LIMIT_ANGLE(f32 x);
void CalcSk1_x(void* dst, void* src, u32 n);
void CalcSk1_x2(void* dst, void* src, u32 n);
typedef f32 Mtx[3][4];
typedef f32 ROMtx[4][3];
extern "C" void PSMTXReorder(const Mtx src, ROMtx dest);

static float frand(float lo, float hi) { return lo + (hi - lo) * (float) (rand() / (double) RAND_MAX); }

static void TestInstructions()
{
    XPs a = xps(1.5f, -2.0f), b = xps(0.25f, 8.0f), c = xps(3.0f, -4.0f);
    XPs r;
    r = ps_add(a, b); CHECK(r.p0 == 1.75f && r.p1 == 6.0f);
    r = ps_sub(a, b); CHECK(r.p0 == 1.25f && r.p1 == -10.0f);
    r = ps_mul(a, c); CHECK(r.p0 == 4.5f && r.p1 == 8.0f);
    r = ps_muls0(a, c); CHECK(r.p0 == 4.5f && r.p1 == -6.0f);          // both halves times c.p0
    r = ps_muls1(a, c); CHECK(r.p0 == -6.0f && r.p1 == 8.0f);          // both halves times c.p1
    r = ps_madd(a, c, b); CHECK(r.p0 == 4.75f && r.p1 == 16.0f);        // a*c + b
    r = ps_msub(a, c, b); CHECK(r.p0 == 4.25f && r.p1 == 0.0f);         // a*c - b
    r = ps_nmadd(a, c, b); CHECK(r.p0 == -4.75f && r.p1 == -16.0f);
    r = ps_nmsub(a, c, b); CHECK(r.p0 == -4.25f && r.p1 == 0.0f);
    r = ps_madds0(a, c, b); CHECK(r.p0 == 1.5f * 3.0f + 0.25f && r.p1 == -2.0f * 3.0f + 8.0f);
    r = ps_madds1(a, c, b); CHECK(r.p0 == 1.5f * -4.0f + 0.25f && r.p1 == -2.0f * -4.0f + 8.0f);
    r = ps_sum0(a, c, b); CHECK(r.p0 == 1.5f + 8.0f && r.p1 == -4.0f);  // (a0 + b1, c1)
    r = ps_sum1(a, c, b); CHECK(r.p0 == 3.0f && r.p1 == 1.5f + 8.0f);   // (c0, a0 + b1)
    r = ps_merge00(a, b); CHECK(r.p0 == 1.5f && r.p1 == 0.25f);
    r = ps_merge01(a, b); CHECK(r.p0 == 1.5f && r.p1 == 8.0f);
    r = ps_merge10(a, b); CHECK(r.p0 == -2.0f && r.p1 == 0.25f);
    r = ps_merge11(a, b); CHECK(r.p0 == -2.0f && r.p1 == 8.0f);
    r = ps_neg(a); CHECK(r.p0 == -1.5f && r.p1 == 2.0f);
    r = ps_abs(a); CHECK(r.p0 == 1.5f && r.p1 == 2.0f);
    r = ps_mr(a); CHECK(r.p0 == 1.5f && r.p1 == -2.0f);
    r = ps_sel(xps(0.0f, -0.0f), c, b); CHECK(r.p0 == 3.0f && r.p1 == 3.0f * 0 + -4.0f);   // >= 0 selects c (-0 too)
    r = ps_sel(xps(-1.0f, NAN), c, b); CHECK(r.p0 == 0.25f && r.p1 == 8.0f);             // negative and NaN select b
    // the multiply-add is fused: a*c rounds only once, with the add
    float e = 1.0f + ldexpf(1.0f, -12);
    r = ps_madd(xps(e, e), xps(e, e), xps(-1.0f, -1.0f));
    CHECK(r.p0 == fmaf(e, e, -1.0f) && r.p0 != e * e - 1.0f);
    r = ps_madds0(xps(e, e), xps(e, 0), xps(-1.0f, -1.0f)); CHECK(r.p0 == fmaf(e, e, -1.0f) && r.p1 == r.p0);
}

static void TestQuantized()
{
    unsigned char buf[16];
    // GQR1 = u8 (scale 0), 2 = s8, 3 = u16 scale 2 / store scale 3, 4 = s16 scale -3, 5 = float
    g_xpsGQR[1] = (4u << 16) | 4u;
    g_xpsGQR[2] = (6u << 16) | 6u;
    g_xpsGQR[3] = (2u << 24) | (5u << 16) | (3u << 8) | 5u;
    g_xpsGQR[4] = ((61u) << 24) | (7u << 16) | (61u << 8) | 7u;           // 61 = -3
    g_xpsGQR[5] = 0;
    buf[0] = 200; buf[1] = 7;
    XPs r = psq_l(buf, 0, 1); CHECK(r.p0 == 200.0f && r.p1 == 7.0f);
    r = psq_l(buf, 1, 1); CHECK(r.p0 == 200.0f && r.p1 == 1.0f);           // W: second value is 1.0
    buf[0] = 0x80; buf[1] = 0x7F;
    r = psq_l(buf, 0, 2); CHECK(r.p0 == -128.0f && r.p1 == 127.0f);        // s8
    uint16_t u[2] = {1000, 7};
    r = psq_l(u, 0, 3); CHECK(r.p0 == 250.0f && r.p1 == 1.75f);            // u16 * 2^-2
    int16_t s[2] = {-5, 100};
    r = psq_l(s, 0, 4); CHECK(r.p0 == -40.0f && r.p1 == 800.0f);           // s16 * 2^3
    float f[2] = {1.5f, -2.25f};
    r = psq_l(f, 0, 5); CHECK(r.p0 == 1.5f && r.p1 == -2.25f);
    r = psq_l(f, 1, 5); CHECK(r.p0 == 1.5f && r.p1 == 1.0f);
    // stores: toward zero, saturating, scaled
    memset(buf, 0xEE, sizeof buf);
    psq_st(xps(2.9f, -2.9f), buf, 0, 2); CHECK((int8_t) buf[0] == 2 && (int8_t) buf[1] == -2 && buf[2] == 0xEE);
    psq_st(xps(300.0f, -300.0f), buf, 0, 2); CHECK((int8_t) buf[0] == 127 && (int8_t) buf[1] == -128);
    psq_st(xps(300.0f, -300.0f), buf, 0, 1); CHECK(buf[0] == 255 && buf[1] == 0);               // u8 clamps both ways
    psq_st(xps(NAN, NAN), buf, 0, 1); CHECK(buf[0] == 255 && buf[1] == 255);
    memset(buf, 0xEE, sizeof buf);
    psq_st(xps(5.0f, 9.0f), buf, 1, 1); CHECK(buf[0] == 5 && buf[1] == 0xEE);                   // W: one value
    int16_t o[2] = {0x1111, 0x1111};
    psq_st(xps(1.0f, 2.0f), o, 0, 3);                                      // u16, store scale 3: *8
    CHECK(o[0] == 8 && o[1] == 16);
    psq_st(xps(1000.0f, -1000.0f), o, 0, 4);                               // s16, store scale -3: /8
    CHECK(o[0] == 125 && o[1] == -125);
    psq_st(xps(1.0e9f, -1.0e9f), o, 0, 4); CHECK(o[0] == 32767 && o[1] == -32768);
    float fo[2] = {0, 0};
    psq_st(xps(1.25f, -3.5f), fo, 0, 5); CHECK(fo[0] == 1.25f && fo[1] == -3.5f);
    fo[1] = 9.0f; psq_st(xps(7.0f, 8.0f), fo, 1, 5); CHECK(fo[0] == 7.0f && fo[1] == 9.0f);
    XPs_InitGQR();
    CHECK(g_xpsGQR[2] == 0x00040004 && g_xpsGQR[3] == 0x00050005 && g_xpsGQR[4] == 0x00060006 && g_xpsGQR[5] == 0x00070007);
}

static void TestReorder()
{
    Mtx m;
    ROMtx ro;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c] = (float) (10 * r + c + 1);   // m[r][c] = 10r + c + 1
    memset(ro, 0, sizeof ro);
    PSMTXReorder(m, ro);
    const float* p = &ro[0][0];
    // six pairs: (m00,m10) (m20,m01) (m11,m21) (m02,m12) (m22,m03) (m13,m23)
    float want[12] = {m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2], m[0][3], m[1][3], m[2][3]};
    CHECK(memcmp(p, want, sizeof want) == 0);
}

static void TestMath()
{
    CHECK(SQRTF(0.0f) == 0.0f && SQRTF(-4.0f) == 0.0f && SQRTF(0.00001f) == 0.0f);   // the early return
    for (float x = 0.001f; x < 100000.0f; x *= 1.37f) {
        float r = SQRTF(x);
        CHECK(fabsf(r - sqrtf(x)) <= 1e-6f * sqrtf(x));
    }
    // LIMIT_ANGLE: the loop of the assembly
    CHECK(LIMIT_ANGLE(0.0f) == 0.0f && LIMIT_ANGLE(1.0f) == 1.0f && LIMIT_ANGLE(-1.0f) == -1.0f);
    CHECK(LIMIT_ANGLE(PI) == PI - PI2);                                       // not below max: brought down
    CHECK(LIMIT_ANGLE(-PI) == -PI);                                           // min itself stays
    CHECK(same(LIMIT_ANGLE(3.5f), 3.5f - PI2) && same(LIMIT_ANGLE(-3.5f), -3.5f + PI2));
    CHECK(same(LIMIT_ANGLE(100.0f), [] { float x = 100.0f; do x -= PI2; while (x >= PI); return x; }()));
    CHECK(same(LIMIT_ANGLE(-100.0f), [] { float x = -100.0f; do x += PI2; while (x < -PI); return x; }()));
    for (int k = -6; k <= 6; k++)                                             // values on and next to the limits
        for (float base : {-PI, PI, -PI - 1e-6f, PI + 1e-6f, 0.5f}) {
            float x = base + (float) k * PI2, ref = x;
            if (!(ref < PI)) { do ref -= PI2; while (ref >= PI); } else if (ref < -PI) { do ref += PI2; while (ref < -PI); }
            CHECK(same(LIMIT_ANGLE(x), ref));
        }
    CHECK(same(LIMIT_ANGLE(-PI - PI2), -PI - PI2 + PI2) || LIMIT_ANGLE(-PI - PI2) >= -PI);
    {   // inputs whose step lands exactly on a limit (x + PI2 == -PI, x - PI2 == PI - PI2 ...): the loop conditions are strict
        int hit = 0;
        float x = nextafterf(-PI - PI2, -100.0f);
        for (int i = 0; i < 128; i++) {
            float ref = x;
            if (!(ref < PI)) { do ref -= PI2; while (ref >= PI); } else if (ref < -PI) { do ref += PI2; while (ref < -PI); }
            CHECK(same(LIMIT_ANGLE(x), ref));
            hit += (x + PI2 == -PI);
            x = nextafterf(x, 0.0f);
        }
        (void) hit;   // (-3*PI is not a float, so no input lands exactly on the limit: `<` and `<=` cannot differ)
    }
    CHECK(isnan(LIMIT_ANGLE(NAN)));                                           // terminates: unordered compares fall through
    for (float x = -50.0f; x < 50.0f; x += 0.173f) { float r = LIMIT_ANGLE(x); CHECK(r >= -PI && r < PI + 1e-3f); }
    // sin / cos: the series against the C library and against the same polynomial evaluated in double
    double worst = 0, worstPoly = 0;
    for (float x = -20.0f; x < 20.0f; x += 0.0137f) {
        float s = SINF(x), c = COSF(x);
        worst = fmax(worst, fmax(fabs((double) s - sin((double) x)), fabs((double) c - cos((double) x))));
        float a = LIMIT_ANGLE(x);
        double p = 0, xx = a;
        double pw = 1.0;
        for (int i = 0; i < 10; i += 2) { pw *= xx; p += Coeff[i] * pw; pw *= xx; p += Coeff[i + 1] * pw; }
        worstPoly = fmax(worstPoly, fabs((double) s - p));
    }
    printf("  max |SINF/COSF - libm| = %g, max |SINF - polynomial| = %g\n", worst, worstPoly);
    CHECK(worst < 3.0e-3);   // the game's own polynomial: accurate to about 2.6e-3 near the ends of the range
    CHECK(worstPoly < 2.0e-6);
    CHECK(fabsf(SINF(0.0f)) < 1e-6f && fabsf(COSF(0.0f) - 1.0f) < 1e-3f);
}

// skinning: reorder + CalcSk1_x must equal M * (x, y, z, 1) on the quantized values
static void TestSkin()
{

    uint8_t* lc = (uint8_t*) XMem_LockedCache();
    // two palette entries
    Mtx m[2];
    for (int k = 0; k < 2; k++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) m[k][r][c] = (float) ((k + 1) * (r + 2) - c);   // small integers: exact
    for (int k = 0; k < 2; k++) PSMTXReorder(m[k], (f32(*)[3]) (lc + 0x30 * k));
    struct V { int16_t x, y, z, idx; } v[3] = {{10, -20, 30, 0}, {-7, 5, 100, 1}, {1000, 2000, -3000, 0}};
    int16_t out[9];
    // GQR6: s16 both ways, no scale -> the transform of the integer values, truncated
    XPS_SET_GQR(6, 0x00070007);
    memset(out, 0x55, sizeof out);
    CalcSk1_x(out, v, 3);
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        for (int r = 0; r < 3; r++) {
            const f32* row = m[v[i].idx][r];
            double e = row[0] * v[i].x + row[1] * v[i].y + row[2] * v[i].z + row[3];
            e = e > 32767 ? 32767 : (e < -32768 ? -32768 : e);
            ok = ok && out[3 * i + r] == (int16_t) e;
        }
    }
    CHECK(ok);
    // with a scale: coordinates are fixed point (value / 2^4 in, * 2^4 out) -> same integers back for an integer matrix
    XPS_SET_GQR(6, (4u << 24) | (4u << 8) | 0x00070007);
    memset(out, 0x55, sizeof out);
    CalcSk1_x(out, v, 3);
    ok = true;
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < 3; r++) {
            const f32* row = m[v[i].idx][r];
            double e = (row[0] * (v[i].x / 16.0) + row[1] * (v[i].y / 16.0) + row[2] * (v[i].z / 16.0) + row[3]) * 16.0;
            e = e > 32767 ? 32767 : (e < -32768 ? -32768 : e);
            ok = ok && out[3 * i + r] == (int16_t) e;
        }
    CHECK(ok);
    // n = 0 touches nothing
    memset(out, 0x55, sizeof out);
    CalcSk1_x(out, v, 0);
    CHECK(out[0] == 0x5555);
    // the s8 normals: 4 bytes in (x y z idx), 3 out; the game sets scale 0x20 both ways (a net scale of 1)
    // (a translation is lost against values scaled by 2^32, as on the console: normals use rotation-only matrices)
    for (int k = 0; k < 2; k++) for (int r = 0; r < 3; r++) m[k][r][3] = 0.0f;
    for (int k = 0; k < 2; k++) PSMTXReorder(m[k], (f32(*)[3]) (lc + 0x30 * k));
    struct N { int8_t x, y, z; uint8_t idx; } nv[2] = {{3, -4, 5, 1}, {-100, 100, 20, 0}};
    int8_t no[6];
    XPS_SET_GQR(6, 0x20062006);
    memset(no, 0x55, sizeof no);
    CalcSk1_x2(no, nv, 2);
    ok = true;
    for (int i = 0; i < 2; i++)
        for (int r = 0; r < 3; r++) {
            const f32* row = m[nv[i].idx][r];
            double e = row[0] * nv[i].x + row[1] * nv[i].y + row[2] * nv[i].z + row[3];
            e = e > 127 ? 127 : (e < -128 ? -128 : e);
            ok = ok && no[3 * i + r] == (int8_t) e;
        }
    CHECK(ok);
    // random float matrices against the double reference (the single precision result may differ by one unit at a boundary)
    srand(7);
    int far = 0;
    for (int t = 0; t < 500; t++) {
        Mtx rm;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) rm[r][c] = frand(-2, 2);
        PSMTXReorder(rm, (f32(*)[3]) (lc + 0x30 * 5));
        V vv = {(int16_t) (rand() % 2000 - 1000), (int16_t) (rand() % 2000 - 1000), (int16_t) (rand() % 2000 - 1000), 5};
        int16_t o3[3];
        XPS_SET_GQR(6, 0x00070007);
        CalcSk1_x(o3, &vv, 1);
        for (int r = 0; r < 3; r++) {
            double e = (double) rm[r][0] * vv.x + (double) rm[r][1] * vv.y + (double) rm[r][2] * vv.z + rm[r][3];
            if (fabs(o3[r] - e) > 1.0) far++;
        }
    }
    CHECK(far == 0);
}

static void TestFmaf()
{
    // XFmaf must equal the correctly rounded libm fmaf: random operands, cancellation, ties, specials
    uint32_t x = 12345u;
    auto rnd = [&]() { x = x * 1664525u + 1013904223u; return x; };
    auto fl = [&](uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; };
    int bad = 0;
    for (int i = 0; i < 2000000; i++) {
        float a = fl(rnd()), b = fl(rnd()), c = fl(rnd());
        if (i & 1) {   // bring the exponents close so the sum cancels / rounds at the edge
            a = fl((rnd() & 0x807FFFFFu) | 0x3F800000u);
            b = fl((rnd() & 0x807FFFFFu) | 0x3F800000u);
            c = -(a * b) + fl(rnd() & 0x33800000u);
        }
        float r = XFmaf(a, b, c), e = fmaf(a, b, c);
        if (!(memcmp(&r, &e, 4) == 0 || (r != r && e != e))) { if (bad++ < 5) printf("fmaf a=%a b=%a c=%a got %a want %a\n", a, b, c, r, e); }
    }
    CHECK(bad == 0);
    CHECK(XFmaf(2.0f, 3.0f, 1.0f) == 7.0f);
    CHECK(XFmaf(1e30f, 1e30f, -1e30f * 1e30f) == fmaf(1e30f, 1e30f, -1e30f * 1e30f));
    float inf = INFINITY;
    CHECK(XFmaf(inf, 2.0f, 1.0f) == inf && XFmaf(0.0f, inf, 1.0f) != XFmaf(0.0f, inf, 1.0f));
}

int main()
{
    XPs_InitGQR();
    TestFmaf();
    TestInstructions();
    TestQuantized();
    TestReorder();
    TestMath();
    TestSkin();
    printf("%d checks, %d failures\n", g_n, g_bad);
    return g_bad != 0;
}
