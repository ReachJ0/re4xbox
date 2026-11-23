// Host test of the software rasterizer (src/xbox/xraster.cpp): the SDK's GX code writes real command streams, the
// processor decodes them, the rasterizer draws into the EFB and the test inspects pixels. RASTER_DUMP=dir writes PPMs.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <dolphin/mtx.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <xbox/xgp.h>
#include <xbox/xmem.h>
#include <xbox/xraster.h>

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned Px(int x, int y) { return XRaster_Pixels()[y * XRASTER_W + x]; }
static int R(unsigned p) { return (p >> 16) & 255; }
static int G(unsigned p) { return (p >> 8) & 255; }
static int B(unsigned p) { return p & 255; }
static int Near(int a, int b, int tol) { return abs(a - b) <= tol; }

static void Dump(const char* name)
{
    const char* dir = getenv("RASTER_DUMP");
    char path[512];
    if (!dir) return;
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d 480\n255\n", XRASTER_W);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < XRASTER_W; x++) {
            unsigned p = Px(x, y);
            fputc(R(p), f); fputc(G(p), f); fputc(B(p), f);
        }
    fclose(f);
}

static void Ortho(float n, float f)   // eye x,y = screen pixels (y down), z in -n..-f
{
    Mtx44 m;
    memset(m, 0, sizeof(m));
    m[0][0] = 2.0f / 640; m[0][3] = -1.0f;
    m[1][1] = -2.0f / 480; m[1][3] = 1.0f;
    m[2][2] = -1.0f / (f - n); m[2][3] = -f / (f - n);
    m[3][3] = 1.0f;
    GXSetProjection(m, GX_ORTHOGRAPHIC);
}

static void Persp(float n, float f)
{
    Mtx44 m;
    memset(m, 0, sizeof(m));
    m[0][0] = 1.0f * 480 / 640; m[1][1] = 1.0f;
    m[2][2] = -n / (f - n); m[2][3] = -f * n / (f - n);
    m[3][2] = -1.0f;
    GXSetProjection(m, GX_PERSPECTIVE);
}

static void Ident()
{
    Mtx m;
    memset(m, 0, sizeof(m));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    GXLoadPosMtxImm(m, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

static void Fmt()
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
}

static void V(float x, float y, float z, int r, int g, int b, int a = 255)
{
    GXPosition3f32(x, y, z);
    GXColor4u8(r, g, b, a);
}

static void Tri(float x0, float y0, float x1, float y1, float x2, float y2, float z, int r, int g, int b, int a = 255)
{
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    V(x0, y0, z, r, g, b, a); V(x1, y1, z, r, g, b, a); V(x2, y2, z, r, g, b, a);
    GXEnd();
    GXFlush();
}

static void Frame(int zEnable = 0, GXCompare fn = GX_ALWAYS)
{
    XRaster_Clear(0xFF000000u, 16777215.0f);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(zEnable, fn, 1);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXFlush();
}

static void TestGouraud()
{
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    V(100, 100, -1, 255, 0, 0); V(300, 100, -1, 0, 255, 0); V(100, 300, -1, 0, 0, 255);
    GXEnd(); GXFlush();
    CHECK(XGp_Pending() == 0);
    CHECK(R(Px(110, 110)) > 200 && G(Px(110, 110)) < 40 && B(Px(110, 110)) < 40);       // near the red corner
    CHECK(G(Px(290, 102)) > 200 && R(Px(290, 102)) < 40);                                 // near the green corner
    CHECK(B(Px(102, 290)) > 200 && R(Px(102, 290)) < 40);                                 // near the blue corner
    CHECK(Near(R(Px(166, 166)), 85, 25) && Near(G(Px(166, 166)), 85, 25) && Near(B(Px(166, 166)), 85, 25));  // centroid
    CHECK(Px(400, 400) == 0xFF000000u && Px(250, 250) == 0xFF000000u && Px(99, 99) == 0xFF000000u);          // outside
    Dump("gouraud");
}

static void TestQuadStripFan()
{
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);       // counter-clockwise on screen
    V(50, 50, -1, 200, 0, 0); V(50, 150, -1, 200, 0, 0); V(150, 150, -1, 200, 0, 0); V(150, 50, -1, 200, 0, 0);
    GXEnd();
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    V(200, 50, -1, 0, 200, 0); V(200, 150, -1, 0, 200, 0); V(300, 50, -1, 0, 200, 0); V(300, 150, -1, 0, 200, 0);
    GXEnd();
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 4);
    V(400, 100, -1, 0, 0, 200); V(350, 50, -1, 0, 0, 200); V(350, 150, -1, 0, 0, 200); V(450, 150, -1, 0, 0, 200);
    GXEnd();
    GXFlush();
    CHECK(R(Px(100, 100)) == 200 && R(Px(60, 140)) == 200 && R(Px(140, 60)) == 200);   // both quad triangles
    CHECK(G(Px(250, 100)) == 200 && G(Px(210, 60)) == 200 && G(Px(290, 140)) == 200);  // both strip triangles
    CHECK(B(Px(380, 100)) == 200 && B(Px(420, 140)) == 200);                           // fan triangles
    CHECK(Px(160, 100) == 0xFF000000u && Px(320, 100) == 0xFF000000u);
    Dump("prims");
}

static void TestDepth()
{
    Frame(1, GX_LESS);
    Ortho(0.1f, 10); Ident(); Fmt();
    Tri(100, 100, 100, 300, 300, 300, -2.0f, 0, 255, 0);     // far, drawn first (counter-clockwise on screen)
    Tri(150, 150, 150, 350, 350, 350, -1.0f, 255, 0, 0);     // near, drawn second
    Tri(120, 80, 120, 240, 280, 240, -3.0f, 0, 0, 255);      // farther than both, must not overwrite
    CHECK(R(Px(160, 300)) == 255 && G(Px(160, 300)) == 0);   // near triangle over the far one
    CHECK(G(Px(110, 200)) == 255 && B(Px(110, 200)) == 0);   // only the far triangle there
    CHECK(Px(125, 220) != 0xFF0000FF);                       // the farthest lost the depth test
    // reversed draw order gives the same picture
    Frame(1, GX_LESS);
    Tri(150, 150, 150, 350, 350, 350, -1.0f, 255, 0, 0);
    Tri(100, 100, 100, 300, 300, 300, -2.0f, 0, 255, 0);
    CHECK(R(Px(160, 300)) == 255 && G(Px(160, 300)) == 0);
    CHECK(G(Px(110, 200)) == 255);
    Dump("depth");
    // z compare off: the later triangle wins whatever its depth
    Frame(0, GX_ALWAYS);
    Tri(150, 150, 150, 350, 350, 350, -1.0f, 255, 0, 0);
    Tri(100, 100, 100, 300, 300, 300, -2.0f, 0, 255, 0);
    CHECK(G(Px(160, 280)) == 255);
}

static void TestCull()
{
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    GXSetCullMode(GX_CULL_BACK);
    Tri(100, 100, 300, 100, 100, 300, -1, 255, 255, 255);     // clockwise on screen = back face
    CHECK(Px(150, 150) == 0xFF000000u);
    Tri(400, 100, 400, 300, 600, 100, -1, 255, 255, 255);     // counter-clockwise = front face
    CHECK(Px(420, 150) != 0xFF000000u);
    GXSetCullMode(GX_CULL_FRONT);
    Tri(100, 350, 300, 350, 100, 450, -1, 0, 255, 255);       // clockwise: now visible
    CHECK(Px(110, 360) != 0xFF000000u);
    Tri(400, 350, 400, 450, 500, 350, -1, 255, 255, 0);       // counter-clockwise: culled
    CHECK(Px(410, 360) == 0xFF000000u);
    GXSetCullMode(GX_CULL_ALL);
    Tri(500, 350, 500, 450, 600, 350, -1, 255, 255, 0);
    CHECK(Px(510, 360) == 0xFF000000u);
    Dump("cull");
}

static void TestBlendScissor()
{
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    Tri(0, 0, 0, 480, 640, 0, -1, 255, 0, 0);                 // red background (upper left half)
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    Tri(50, 50, 50, 250, 250, 50, -1, 0, 0, 255, 128);        // 50% blue over red
    CHECK(Near(R(Px(80, 80)), 127, 3) && Near(B(Px(80, 80)), 128, 3));
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
    Tri(50, 50, 50, 250, 250, 50, -1, 0, 100, 0);             // additive green
    CHECK(Near(G(Px(80, 80)), 100, 2) && Near(R(Px(80, 80)), 127, 3));
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetScissor(300, 200, 100, 100);
    GXFlush();
    Tri(250, 150, 250, 450, 550, 150, -1, 255, 255, 255);     // only x 300..399, y 200..299 may change
    CHECK(Px(350, 250) == 0xFFFFFFFFu);
    CHECK(Px(299, 250) != 0xFFFFFFFFu && Px(400, 250) != 0xFFFFFFFFu && Px(350, 199) != 0xFFFFFFFFu && Px(350, 300) != 0xFFFFFFFFu);
    Dump("blend");
}

static void TestPerspectiveAndClip()
{
    Frame(1, GX_LESS);
    Persp(0.1f, 100);
    Ident(); Fmt();
    // a triangle 2 units away, centred: the viewport maps x -1..1 (aspect 480/640 -> width 0.75..) onto the screen
    Tri(-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f, -2.0f, 255, 128, 0);
    CHECK(Px(320, 290) != 0xFF000000u && Px(320, 200) != 0xFF000000u);   // inside (apex at y 180, base at y 300)
    CHECK(Px(320, 120) == 0xFF000000u);                       // above the apex
    CHECK(Px(10, 10) == 0xFF000000u);
    // a floor triangle running from in front of the camera to behind it must be clipped, not smeared
    Frame(1, GX_LESS);
    // (the camera looks down -z; the next triangle starts 2 units ahead and ends 5 units behind it)
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    V(-1.0f, -1.0f, -2.0f, 255, 255, 255); V(1.0f, -1.0f, -2.0f, 255, 255, 255); V(0.0f, -1.0f, 5.0f, 255, 255, 255);
    GXEnd(); GXFlush();
    CHECK(XGp_Pending() == 0);
    int lit = 0;
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) if (Px(x, y) != 0xFF000000u) lit++;
    CHECK(lit > 0 && lit < 640 * 480);
    Dump("persp");
    // everything behind the camera draws nothing
    Frame(1, GX_LESS);
    Tri(-1, -1, 1, -1, 0, 1, 2.0f, 255, 255, 255);
    CHECK(Px(320, 240) == 0xFF000000u && XRaster_Pixels()[0] == 0xFF000000u);
}

static void TestLinesPoints()
{
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    GXBegin(GX_LINES, GX_VTXFMT0, 2);
    V(10, 10, -1, 255, 255, 255); V(110, 10, -1, 255, 255, 255);
    GXEnd();
    GXBegin(GX_POINTS, GX_VTXFMT0, 1);
    V(200, 200, -1, 255, 0, 255);
    GXEnd(); GXFlush();
    CHECK(Px(60, 10) == 0xFFFFFFFFu && Px(60, 12) == 0xFF000000u);
    CHECK(Px(200, 200) == 0xFFFF00FFu);
}

static void TestCopyDisp(unsigned char* blk)
{
    unsigned char* xfb = blk + 0x200000;
    Frame();
    Ortho(0.1f, 10); Ident(); Fmt();
    Tri(0, 0, 0, 480, 640, 0, -1, 255, 0, 0);                 // red upper left half
    Tri(640, 480, 640, 0, 0, 480, -1, 0, 0, 255);             // blue lower right half
    unsigned before = XRaster_CopySerial();
    GXSetDispCopySrc(0, 0, 640, 480);
    GXSetDispCopyDst(640, 480);
    GXSetCopyClear((GXColor){0, 255, 0, 255}, 0x00FFFFFF);
    GXCopyDisp(xfb, GX_TRUE);
    GXFlush();
    CHECK(XGp_Pending() == 0);
    CHECK(XRaster_CopySerial() == before + 1);
    // pixel (100,50): red -> Y U Y V bytes at row 50, x pair 50
    const unsigned char* a = xfb + 50 * 1280 + 50 * 4;
    CHECK(Near(a[0], 82, 3) && Near(a[2], 82, 3) && Near(a[1], 90, 3) && Near(a[3], 240, 3));
    // pixel (500,400): blue
    const unsigned char* b = xfb + 400 * 1280 + 250 * 4;
    CHECK(Near(b[0], 41, 3) && Near(b[1], 240, 3) && Near(b[3], 110, 3));
    // the copy cleared the EFB to the clear colour (green) after copying
    CHECK(Px(100, 50) == 0xFF00FF00u && Px(500, 400) == 0xFF00FF00u && XRaster_Depth()[50 * XRASTER_W + 100] == 16777215u);
    // vertical scale 1.5: 320 source lines become 480 lines; line 3 of the copy comes from source line 2
    Frame();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    V(0, 0, -1, 255, 255, 255); V(0, 160, -1, 255, 255, 255); V(640, 160, -1, 255, 255, 255); V(640, 0, -1, 255, 255, 255);
    GXEnd();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    V(0, 160, -1, 0, 0, 0); V(0, 320, -1, 0, 0, 0); V(640, 320, -1, 0, 0, 0); V(640, 160, -1, 0, 0, 0);
    GXEnd();
    GXFlush();
    memset(xfb, 0, 1280 * 490);
    GXSetDispCopySrc(0, 0, 640, 320);
    GXSetDispCopyDst(640, 480);
    GXSetDispCopyYScale(1.5f);
    GXCopyDisp(xfb, GX_FALSE);
    GXFlush();
    CHECK(Near(xfb[200 * 1280], 235, 3));                    // line 200 -> source line 133: still white
    CHECK(Near(xfb[260 * 1280], 16, 3));                     // line 260 -> source line 173: black
    CHECK(xfb[480 * 1280] != 0 && xfb[481 * 1280] == 0);     // 481 lines (what GXSetDispCopyYScale reports for 320 lines at 1.5)
    CHECK(Px(10, 10) == 0xFFFFFFFFu);                        // no clear this time
    GXSetDispCopyYScale(1.0f);
}

// ---- textures and TEV ----
static void TFmt()
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

// full-screen-ish quad (100,100)-(500,500) showing the whole texture, vertex colour (r,g,b,a)
static void TexQuad(int r, int g, int b, int a)
{
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(100, 100, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(0, 0);
    GXPosition3f32(500, 100, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(1, 0);
    GXPosition3f32(500, 500, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(1, 1);
    GXPosition3f32(100, 500, -1); GXColor4u8(r, g, b, a); GXTexCoord2f32(0, 1);
    GXEnd();
    GXFlush();
}

static void UseTex(void* data, int w, int h, GXTexFmt fmt, GXTevMode mode)
{
    // the SDK keeps a 32 byte object on the console (32 bit pointers); on a 64 bit host its internal struct is 40 bytes
    static unsigned long long objbuf[8];
    GXTexObj& obj = *(GXTexObj*) objbuf;
    GXInitTexObj(&obj, data, w, h, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, mode);
    GXFlush();
}

static unsigned char* mem_for_copy(unsigned char* t) { return t + 0x40000; }

static void TestTextures(unsigned char* mem)
{
    unsigned char* t = mem + 0x200000;   // 32-byte aligned texture data in game memory
    Frame();
    Ortho(0.1f, 10); Ident(); TFmt();

    // RGBA8 4x4: one block, AR bytes then GB bytes; texel (x,y): R = 60*x, G = 60*y, B = 7, A = 255 - 10*x
    memset(t, 0, 64);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int k = (y * 4 + x) * 2;
            t[k] = 255 - 10 * x; t[k + 1] = 60 * x; t[32 + k] = 60 * y; t[32 + k + 1] = 7;
        }
    UseTex(t, 4, 4, GX_TF_RGBA8, GX_REPLACE);
    TexQuad(255, 255, 255, 255);
    CHECK(Near(R(Px(150, 150)), 0, 1) && Near(G(Px(150, 150)), 0, 1) && Near(B(Px(150, 150)), 7, 1));
    CHECK(Near(R(Px(250, 150)), 60, 1) && Near(G(Px(250, 150)), 0, 1));
    CHECK(Near(R(Px(450, 150)), 180, 1) && Near(G(Px(450, 350)), 120, 1) && Near(G(Px(150, 450)), 180, 1));
    Dump("tex_rgba8");

    // MODULATE: texel * vertex colour / 255
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    UseTex(t, 4, 4, GX_TF_RGBA8, GX_MODULATE);
    TexQuad(128, 255, 255, 255);
    CHECK(Near(R(Px(450, 150)), 90, 2) && Near(B(Px(450, 150)), 7, 1));

    // I8 8x4, texel (x,y) = 30*x + y
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) t[y * 8 + x] = (unsigned char) (30 * x + y);
    UseTex(t, 8, 4, GX_TF_I8, GX_REPLACE);
    TexQuad(255, 255, 255, 255);
    CHECK(Near(R(Px(125, 150)), 0, 1) && Near(R(Px(475, 150)), 210, 1) && Near(R(Px(125, 450)), 3, 1) && Near(G(Px(275, 250)), 91, 1));

    // IA8 4x4 with alpha test: alpha 0 texels must not be drawn
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) { t[(y * 4 + x) * 2] = (x & 1) ? 255 : 0; t[(y * 4 + x) * 2 + 1] = 200; }
    UseTex(t, 4, 4, GX_TF_IA8, GX_REPLACE);
    GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
    GXFlush();
    TexQuad(255, 255, 255, 255);
    CHECK(Px(150, 150) == 0xFF000000u && R(Px(250, 150)) == 200 && Px(350, 350) == 0xFF000000u && R(Px(450, 350)) == 200);

    // CMPR 8x8: four 4x4 blocks with the same data: colour 0 red, colour 1 blue, one row of each index
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    memset(t, 0, 32);
    for (int b = 0; b < 4; b++) {
        unsigned char* q = t + b * 8;
        q[0] = 0xF8; q[1] = 0x00; q[2] = 0x00; q[3] = 0x1F;
        q[4] = 0x00; q[5] = 0x55; q[6] = 0xAA; q[7] = 0xFF;
    }
    UseTex(t, 8, 8, GX_TF_CMPR, GX_REPLACE);
    TexQuad(255, 255, 255, 255);
    CHECK(R(Px(120, 120)) == 255 && B(Px(120, 120)) == 0);                       // row 0: colour 0
    CHECK(R(Px(120, 170)) == 0 && B(Px(120, 170)) == 255);                       // row 1: colour 1
    CHECK(Near(R(Px(120, 220)), 170, 2) && Near(B(Px(120, 220)), 85, 2));        // row 2: 2/3 c0 + 1/3 c1
    CHECK(Near(R(Px(120, 270)), 85, 2) && Near(B(Px(120, 270)), 170, 2));        // row 3
    CHECK(R(Px(420, 320)) == 255);                                               // second sub-block row 4 (index 0)
    Dump("tex_cmpr");

    // C8 8x4 with an RGB565 palette (red, green, blue, white), texel index = x % 4; then the palette is changed under the same texture
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    {
        unsigned char* pal = t + 0x1000;
        memset(t, 0, 32);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) t[y * 8 + x] = (unsigned char) (x % 4);
        memset(pal, 0, 64);
        unsigned short cols[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        for (int k = 0; k < 4; k++) { pal[k * 2] = cols[k] >> 8; pal[k * 2 + 1] = cols[k] & 255; }
        static unsigned long long objbuf[8], tlbuf[8];
        GXTexObj& obj = *(GXTexObj*) objbuf;
        GXTlutObj& tl = *(GXTlutObj*) tlbuf;
        GXInitTlutObj(&tl, pal, GX_TL_RGB565, 16);
        GXLoadTlut(&tl, GX_TLUT0);
        GXInitTexObjCI(&obj, t, 8, 4, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
        GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&obj, GX_TEXMAP0);
        GXSetNumTexGens(1);
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
        GXFlush();
        TexQuad(255, 255, 255, 255);
        CHECK(R(Px(100, 150)) == 255 && G(Px(100, 150)) == 0);       // x 0 -> red
        CHECK(G(Px(170, 150)) == 255 && R(Px(170, 150)) == 0);       // x 1 -> green
        CHECK(B(Px(240, 150)) == 255 && G(Px(240, 150)) == 0);       // x 2 -> blue
        CHECK(R(Px(270, 150)) == 255 && G(Px(270, 150)) == 255);     // x 3 -> white
        Dump("tex_c8");
        pal[0] = 0x07; pal[1] = 0xE0;                                 // colour 0 becomes green: the cache must notice
        GXLoadTlut(&tl, GX_TLUT0);
        GXFlush();
        TexQuad(255, 255, 255, 255);
        CHECK(G(Px(100, 150)) == 255 && R(Px(100, 150)) == 0);
    }

    // GXCopyTex: the EFB region lands in the tiled texture layout (RGBA8 block = 4x4 texels, AR bytes then GB bytes), also halved
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) t[y * 8 + x] = (unsigned char) (30 * x + y);
    UseTex(t, 8, 4, GX_TF_I8, GX_REPLACE);
    TexQuad(255, 255, 255, 255);
    {
        unsigned char* d = mem_for_copy(t);
        GXSetTexCopySrc(300, 300, 64, 64);
        GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
        GXCopyTex(d, GX_FALSE);
        GXFlush();
        // texel (5,6) is in block (1,1), position (1,2) inside it
        unsigned char* blk = d + ((1 * 16 + 1) * 64);
        int k = (2 * 4 + 1) * 2;
        unsigned px = Px(305, 306);
        CHECK(R(px) != 0 || G(px) != 0);
        CHECK(blk[k + 1] == R(px) && blk[32 + k] == G(px) && blk[32 + k + 1] == B(px));
        CHECK(blk[k] == (px >> 24));
        GXSetTexCopyDst(32, 32, GX_TF_I8, GX_TRUE);
        GXCopyTex(d, GX_FALSE);
        GXFlush();
        // 32x32 I8 texels in 8x4 blocks (4 blocks per row of 32): texel (3,2) = average of EFB (6..7, 4..5), luminance
        unsigned lum = 0;
        for (int q = 0; q < 4; q++) { unsigned p = Px(306 + (q & 1), 304 + (q >> 1)); lum += (R(p) * 77 + G(p) * 150 + B(p) * 29) >> 8; }
        CHECK(Near(d[(0 * 4 + 0) * 32 + 2 * 8 + 3], (int) (lum / 4), 2));
    }

    // draw capture: with a clock that makes every draw look slow, the next draw is written to a file the replay tool reads
    {
        static unsigned long long fake;
        struct F { static unsigned long long Now() { return fake += 2000000ull; } };
        const char* capPath = getenv("RASTER_CAPTURE");
        if (capPath) {
            XRaster_SetClock(F::Now);
            XRaster_SetCapture(capPath);
            Frame(); Ortho(0.1f, 10); Ident(); TFmt();
            UseTex(t, 8, 4, GX_TF_I8, GX_REPLACE);
            TexQuad(255, 255, 255, 255);
            XRaster_SetCapture(0);
            XRaster_SetClock(0);
            FILE* cf = fopen(capPath, "rb");
            unsigned magic = 0;
            CHECK(cf && fread(&magic, 4, 1, cf) == 1 && magic == 0x50414352u);
            if (cf) fclose(cf);
        }
    }

    // restore the plain vertex-colour state for anything after this
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXFlush();
}

#include <time.h>
static double Now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

// RASTER_BENCH=1: milliseconds for the heavy parts of one game frame (the console runs it on a much slower CPU)
static void Bench(unsigned char* mem)
{
    unsigned char* t = mem + 0x200000;
    memset(t, 0x55, 64 * 1024);
    Frame(); Ortho(0.1f, 10); Ident(); TFmt();
    double a = Now();
    UseTex(t, 4, 4, GX_TF_RGBA8, GX_MODULATE);
    for (int i = 0; i < 5; i++) TexQuad(255, 255, 255, 255);
    double b = Now();
    printf("bench: textured RGBA8 nearest quad 400x400:  %.2f ms\n", (b - a) / 5);
    UseTex(t, 256, 256, GX_TF_CMPR, GX_MODULATE);
    for (int i = 0; i < 5; i++) TexQuad(255, 255, 255, 255);
    printf("bench: textured CMPR 256x256 nearest quad:   %.2f ms\n", (Now() - b) / 5);
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXFlush();
    b = Now();
    for (int i = 0; i < 5; i++) TexQuad(255, 255, 255, 255);
    printf("bench: vertex colour quad 400x400 (TEV):     %.2f ms\n", (Now() - b) / 5);
    b = Now();
    for (int i = 0; i < 5; i++) { GXSetCopyClear(GXColor{0, 0, 0, 255}, 0xFFFFFF); GXCopyDisp(mem + 0x300000, GX_TRUE); GXFlush(); }
    printf("bench: GXCopyDisp 640x480 with clear:        %.2f ms\n", (Now() - b) / 5);
}

int main()
{
    void* want = (void*) (uintptr_t) XMEM_WANTED_BASE;
    void* blk = mmap(want, XMEM_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_32BIT, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); return 2; }
    CHECK(XMem_SetBase(blk, XMEM_SPAN));
    GXInit((unsigned char*) blk + 0x100000, 0x20000);
    XRaster_Init();
    if (getenv("RASTER_FLOAT")) XRaster_SetFixed(0);
    XGp_SetBackend(XRaster_Backend());
    TestGouraud();
    TestQuadStripFan();
    TestDepth();
    TestCull();
    TestBlendScissor();
    TestPerspectiveAndClip();
    TestLinesPoints();
    TestCopyDisp((unsigned char*) blk);
    TestTextures((unsigned char*) blk);
    if (getenv("RASTER_BENCH")) Bench((unsigned char*) blk);
    printf("raster: %d checks, %d failures, %llu triangles\n", g_checks, g_fails, XRaster_Triangles());
    return g_fails != 0;
}
