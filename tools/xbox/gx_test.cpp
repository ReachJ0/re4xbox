// Host test of the GX layer: the SDK's own GX code (src/lib/GX*.c) drives the software command processor.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <xbox/xgp.h>
#include <xbox/xmem.h>

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b) (fabsf((float) (a) - (float) (b)) < 1e-5f)

// ---- recorder backend
struct Draw { int prim, vat, count; XGpVertex v[16]; };
static Draw g_draws[32];
static int g_nDraws, g_tokenCalls, g_lastToken, g_finishCalls;
static void OnDraw(int prim, int vat, const XGpVertex* v, int count, const XGpState*, void*)
{
    if (g_nDraws < 32) {
        Draw& d = g_draws[g_nDraws];
        d.prim = prim; d.vat = vat; d.count = count;
        memcpy(d.v, v, (count > 16 ? 16 : count) * sizeof(XGpVertex));
    }
    g_nDraws++;
}
static void OnFinish(void*) { g_finishCalls++; }
static void OnToken(unsigned short t, void*) { g_tokenCalls++; g_lastToken = t; }
static XGpBackend g_be = {OnDraw, 0, OnFinish, OnToken, 0};

static unsigned char* g_blk;
static void* P(unsigned off) { return g_blk + off; }
static unsigned FBits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
static void PutBE32(unsigned off, unsigned v) { unsigned char* p = g_blk + off; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void PutBEF(unsigned off, float f) { PutBE32(off, FBits(f)); }

static const XGpState* S() { return XGp_State(); }
static void NewScene() { g_nDraws = 0; }

static void TestInit()
{
    GXFifoObj* f = GXInit(P(0x100000), 0x20000);
    CHECK(f != 0);
    CHECK(XGp_Pending() == 0);                 // every command the SDK wrote was complete and understood
    CHECK(S()->cmds > 20);
    GXFlush();
    CHECK(XGp_Pending() == 0);
}

static void TestVertexFormats()
{
    XGp_SetBackend(&g_be);
    // pos f32 xyz, color rgba8, tex f32 st
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXFlush();
    CHECK(XGp_VertexSize(0) == 12 + 4 + 8);
    NewScene();
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) {
        GXPosition3f32(1.5f + i, -2.0f * i, 0.25f);
        GXColor4u8(10 + i, 20 + i, 30 + i, 200 + i);
        GXTexCoord2f32(0.5f * i, 1.0f - i);
    }
    GXEnd();
    GXFlush();
    CHECK(XGp_Pending() == 0);
    CHECK(g_nDraws == 1 && g_draws[0].prim == XGP_TRIANGLES && g_draws[0].count == 3 && g_draws[0].vat == 0);
    for (int i = 0; i < 3 && g_nDraws == 1; i++) {
        const XGpVertex& v = g_draws[0].v[i];
        CHECK(NEAR(v.pos[0], 1.5f + i) && NEAR(v.pos[1], -2.0f * i) && NEAR(v.pos[2], 0.25f));
        CHECK(v.col[0][0] == 10 + i && v.col[0][1] == 20 + i && v.col[0][2] == 30 + i && v.col[0][3] == 200 + i);
        CHECK(NEAR(v.tex[0][0], 0.5f * i) && NEAR(v.tex[0][1], 1.0f - i));
        CHECK((v.present & (XGP_HAS_COL0 | XGP_HAS_TEX0)) == (XGP_HAS_COL0 | XGP_HAS_TEX0) && !(v.present & XGP_HAS_NRM));
    }
}

static void TestPrimitives()
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    static const GXPrimitive prim[7] = {GX_QUADS, GX_TRIANGLES, GX_TRIANGLESTRIP, GX_TRIANGLEFAN, GX_LINES, GX_LINESTRIP, GX_POINTS};
    static const int op[7] = {XGP_QUADS, XGP_TRIANGLES, XGP_TRISTRIP, XGP_TRIFAN, XGP_LINES, XGP_LINESTRIP, XGP_POINTS};
    NewScene();
    for (int k = 0; k < 7; k++) {
        GXBegin(prim[k], GX_VTXFMT3, 4);
        for (int i = 0; i < 4; i++) { GXPosition2f32((float) i, (float) k); }
        GXEnd();
    }
    GXFlush();
    CHECK(g_nDraws == 7);
    for (int k = 0; k < 7 && k < g_nDraws; k++) {
        CHECK(g_draws[k].prim == op[k] && g_draws[k].vat == 3 && g_draws[k].count == 4);
        CHECK(NEAR(g_draws[k].v[3].pos[0], 3) && NEAR(g_draws[k].v[3].pos[1], k));
    }
    CHECK(XGp_Pending() == 0);
}

static void TestFixedPoint()
{
    // position s16 frac 8, u8 frac 2; normal s8; tex s16 frac 7; colors rgb565 / rgb8 / rgba4 / rgba6
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_S16, 7);
    GXFlush();
    CHECK(XGp_VertexSize(1) == 6 + 3 + 2 + 4);
    NewScene();
    GXBegin(GX_POINTS, GX_VTXFMT1, 1);
    GXPosition3s16(896, -256, 1);               // 3.5, -1.0, 1/256
    GXNormal3s8(64, -64, 32);                   // 1.0, -1.0, 0.5
    GXColor1u16(0xF81F);                        // r=31 g=0 b=31
    GXTexCoord2s16(128, -64);                   // 1.0, -0.5
    GXEnd();
    GXFlush();
    CHECK(g_nDraws == 1);
    if (g_nDraws == 1) {
        const XGpVertex& v = g_draws[0].v[0];
        CHECK(NEAR(v.pos[0], 3.5f) && NEAR(v.pos[1], -1.0f) && NEAR(v.pos[2], 1.0f / 256));
        CHECK(NEAR(v.nrm[0], 1.0f) && NEAR(v.nrm[1], -1.0f) && NEAR(v.nrm[2], 0.5f));
        CHECK(v.col[0][0] == 255 && v.col[0][1] == 0 && v.col[0][2] == 255 && v.col[0][3] == 255);
        CHECK(NEAR(v.tex[0][0], 1.0f) && NEAR(v.tex[0][1], -0.5f));
    }
    // u8 positions with frac 2, RGB8 and RGBA4 colour, u8 tex coords
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XY, GX_U8, 2);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGB, GX_RGB8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA4, 0);
    GXFlush();
    CHECK(XGp_VertexSize(2) == 2 + 3 + 2);
    NewScene();
    GXBegin(GX_POINTS, GX_VTXFMT2, 1);
    GXPosition2u8(10, 255);                     // 2.5, 63.75
    GXColor3u8(1, 2, 3);
    GXColor1u16(0xF0A5);                        // r=15 g=0 b=10 a=5 (x17)
    GXEnd();
    GXFlush();
    CHECK(g_nDraws == 1);
    if (g_nDraws == 1) {
        const XGpVertex& v = g_draws[0].v[0];
        CHECK(NEAR(v.pos[0], 2.5f) && NEAR(v.pos[1], 63.75f));
        CHECK(v.col[0][0] == 1 && v.col[0][1] == 2 && v.col[0][2] == 3);
        CHECK(v.col[1][0] == 255 && v.col[1][1] == 0 && v.col[1][2] == 170 && v.col[1][3] == 85);
    }
}

static void TestIndexed()
{
    // arrays in memory are big endian
    const unsigned posOff = 0x200000, clrOff = 0x201000, mtxIdxOff = 0x202000;
    for (int i = 0; i < 8; i++) { PutBEF(posOff + 16 * i, 1.0f * i); PutBEF(posOff + 16 * i + 4, 2.0f * i); PutBEF(posOff + 16 * i + 8, 3.0f * i); }
    for (int i = 0; i < 8; i++) { unsigned char* p = g_blk + clrOff + 4 * i; p[0] = i; p[1] = 2 * i; p[2] = 3 * i; p[3] = 255 - i; }
    (void) mtxIdxOff;
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetArray(GX_VA_POS, P(posOff), 16);
    GXSetArray(GX_VA_CLR0, P(clrOff), 4);
    GXFlush();
    CHECK(XGp_VertexSize(4) == 1 + 2 + 1);
    NewScene();
    GXBegin(GX_TRIANGLES, GX_VTXFMT4, 3);
    for (int i = 0; i < 3; i++) {
        GXMatrixIndex1u8(3 * (i + 1));
        GXPosition1x16(7 - i);
        GXColor1x8(i + 2);
    }
    GXEnd();
    GXFlush();
    CHECK(g_nDraws == 1);
    for (int i = 0; i < 3 && g_nDraws == 1; i++) {
        const XGpVertex& v = g_draws[0].v[i];
        CHECK(v.posMtxIdx == 3 * (i + 1) && (v.present & XGP_HAS_POSMTX));
        CHECK(NEAR(v.pos[0], 7 - i) && NEAR(v.pos[1], 2.0f * (7 - i)) && NEAR(v.pos[2], 3.0f * (7 - i)));
        CHECK(v.col[0][0] == i + 2 && v.col[0][1] == 2 * (i + 2) && v.col[0][3] == 255 - (i + 2));
    }
}

static void TestMatrices()
{
    f32 m[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c] = 10.0f * r + c + 0.5f;
    GXLoadPosMtxImm(m, GX_PNMTX1);
    GXFlush();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) CHECK(S()->xf[12 + r * 4 + c] == FBits(m[r][c]));
    f32 n[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) n[r][c] = 100.0f + 10 * r + c;
    GXLoadNrmMtxImm(n, GX_PNMTX1);
    GXFlush();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) CHECK(S()->xf[0x400 + 9 + r * 3 + c] == FBits(n[r][c]));
    f32 t[2][4];
    for (int r = 0; r < 2; r++) for (int c = 0; c < 4; c++) t[r][c] = 7.0f * r + c;
    GXLoadTexMtxImm(t, GX_TEXMTX0 + 3 * 2, GX_MTX2x4);
    GXFlush();
    for (int r = 0; r < 2; r++) for (int c = 0; c < 4; c++) CHECK(S()->xf[(GX_TEXMTX0 + 6) * 4 + r * 4 + c] == FBits(t[r][c]));
}

static void TestViewportProjection()
{
    GXSetViewport(10.0f, 20.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXFlush();
    // scale x/y/z, offset x/y/z (XF 0x101A..0x101F)
    CHECK(S()->xf[0x101A] == FBits(320.0f) && S()->xf[0x101B] == FBits(-240.0f));
    CHECK(S()->xf[0x101D] == FBits(10.0f + 320.0f + 342.0f) && S()->xf[0x101E] == FBits(20.0f + 240.0f + 342.0f));
    CHECK(NEAR(*(float*) &S()->xf[0x101C], 16777215.0f) || NEAR(*(float*) &S()->xf[0x101C], 16777216.0f));
    f32 p[4][4] = {{1.5f, 0, 0.25f, 0}, {0, 2.5f, -0.5f, 0}, {0, 0, -1.01f, -0.2f}, {0, 0, -1, 0}};
    GXSetProjection(p, GX_PERSPECTIVE);
    GXFlush();
    CHECK(S()->xf[0x1020] == FBits(1.5f) && S()->xf[0x1021] == FBits(0.25f));
    CHECK(S()->xf[0x1022] == FBits(2.5f) && S()->xf[0x1023] == FBits(-0.5f));
    CHECK(S()->xf[0x1024] == FBits(-1.01f) && S()->xf[0x1025] == FBits(-0.2f));
    CHECK(S()->xf[0x1026] == 0);                // perspective
    f32 o[4][4] = {{2, 0, 0, -1}, {0, 3, 0, 0.5f}, {0, 0, -0.5f, -1.5f}, {0, 0, 0, 1}};
    GXSetProjection(o, GX_ORTHOGRAPHIC);
    GXFlush();
    CHECK(S()->xf[0x1020] == FBits(2.0f) && S()->xf[0x1021] == FBits(-1.0f) && S()->xf[0x1022] == FBits(3.0f));
    CHECK(S()->xf[0x1023] == FBits(0.5f) && S()->xf[0x1024] == FBits(-0.5f) && S()->xf[0x1025] == FBits(-1.5f));
    CHECK(S()->xf[0x1026] == 1);
}

static void TestBpState()
{
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXFlush();
    CHECK((S()->bp[0x40] & 1) && ((S()->bp[0x40] >> 1) & 7) == GX_LEQUAL && ((S()->bp[0x40] >> 4) & 1));
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXFlush();
    CHECK(!(S()->bp[0x40] & 1) && ((S()->bp[0x40] >> 1) & 7) == GX_ALWAYS && !((S()->bp[0x40] >> 4) & 1));
    GXSetScissor(5, 6, 100, 50);
    GXFlush();
    CHECK(S()->bp[0x20] == (((5 + 342) << 12) | (6 + 342)) && S()->bp[0x21] == (((5 + 100 - 1 + 342) << 12) | (6 + 50 - 1 + 342)));
    GXSetNumChans(2);
    GXSetNumTexGens(3);
    GXSetNumTevStages(4);
    GXFlush();
    CHECK(S()->xf[0x1009] == 2 && S()->xf[0x103F] == 3);
}

static void TestDisplayList()
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXFlush();
    unsigned char* dl = g_blk + 0x300000;
    NewScene();
    GXBeginDisplayList(dl, 0x1000);
    GXBegin(GX_TRIANGLES, GX_VTXFMT5, 3);
    for (int i = 0; i < 3; i++) GXPosition3f32(4.0f + i, 5.0f, 6.0f);
    GXEnd();
    u32 size = GXEndDisplayList();
    CHECK(g_nDraws == 0);                       // recording does not execute
    CHECK(size >= 3 + 36 + 1 && size % 32 == 0);
    unsigned first = 0;
    while (first < size && dl[first] == 0) first++;     // the write-gather pipe may hold NOP padding from before the list
    CHECK(first < 32 && dl[first] == (XGP_TRIANGLES | 5) && dl[first + 1] == 0 && dl[first + 2] == 3);
    GXCallDisplayList(dl, size);
    GXFlush();
    CHECK(g_nDraws == 1 && g_draws[0].count == 3 && g_draws[0].vat == 5 && NEAR(g_draws[0].v[2].pos[0], 6.0f));
    GXCallDisplayList(dl, size);
    GXFlush();
    CHECK(g_nDraws == 2);                       // and it can be replayed
    CHECK(XGp_Pending() == 0);
}

static void TestSync()
{
    g_tokenCalls = 0; g_finishCalls = 0;
    GXSetDrawSync(0x1234);
    GXFlush();
    CHECK(GXReadDrawSync() == 0x1234 && g_tokenCalls == 1 && g_lastToken == 0x1234);
    GXDrawDone();                               // must not block: the processor is synchronous
    CHECK(g_finishCalls == 1);
}

int main()
{
    void* want = (void*) (uintptr_t) XMEM_WANTED_BASE;
    void* blk = mmap(want, XMEM_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_32BIT, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); return 2; }
    CHECK(XMem_SetBase(blk, XMEM_SPAN));
    g_blk = (unsigned char*) blk;
    TestInit();
    TestVertexFormats();
    TestPrimitives();
    TestFixedPoint();
    TestIndexed();
    TestMatrices();
    TestViewportProjection();
    TestBpState();
    TestDisplayList();
    TestSync();
    printf("gx: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
