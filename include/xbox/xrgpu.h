// Hook that lets a GPU backend (NV2A, src/xbox/xnv2a.cpp) replace the pixel work of the software rasterizer (xraster.cpp).
// The rasterizer keeps doing what it already does well on the CPU: vertex transform, near/far clipping, viewport,
// culling, decoding GX textures to ARGB, and the encoding of EFB copies. The backend only receives finished triangles in
// window coordinates plus the pixel state (TEV, blend, depth, alpha compare, scissor) and draws them on the GPU.
#ifndef XBOX_XRGPU_H
#define XBOX_XRGPU_H

#ifdef __cplusplus
extern "C" {
#endif

// One vertex: x, y in window pixels (EFB coordinates), z in the 24 bit depth units of the GX viewport (0..16777215),
// w = clip-space w (> 0, for perspective correct interpolation), colour 0..1, texture coordinates normalised as GX gives them.
typedef struct {
    float x, y, z, w;
    float c[4];
    float tc[4][2];
} XRGpuVert;

// A decoded GX texture (level 0 as 0xAARRGGBB). id/ver identify the content: same id and ver = same texels.
typedef struct {
    const unsigned* px;   // 0: not available (the backend binds a white texel)
    int w, h;
    unsigned id, ver;
    int wrapS, wrapT;     // GX: 0 clamp, 1 repeat, 2 mirror
    int linear;           // bilinear filter
} XRGpuTex;

// One TEV stage as GX encodes it (selectors 0..15 as in the GX_CC_* / GX_CA_* enums)
typedef struct {
    int cmap, ccoord, cchan;   // texture map (-1 none), texture coordinate set, raster channel (-1 = none)
    int ca, cb, cc, cd, cbias, cop, cclamp, cscale, cdest;
    int aa, ab, ac, ad, abias, aop, aclamp, ascale, adest;
    float kcv[3];              // the selected colour constant, 0..255
    float kav;                 // the selected alpha constant, 0..255
    int needTex;
} XRGpuStage;

typedef struct {
    int x0, y0, x1, y1;           // scissor, inclusive
    int zEnable, zFunc, zUpdate;  // GX compare 0 never .. 7 always
    unsigned blend;               // BP 0x41
    int aTest, acomp0, acomp1, alogic, aref0, aref1;
    int tev, nstage;              // tev = 0: the TEV was never programmed (draw the vertex colour)
    XRGpuStage stage[16];
    float reg[4][4];              // initial TEV registers prev, c0, c1, c2 (r g b a, 0..255)
    XRGpuTex tex[8];
} XRGpuBatch;

typedef struct {
    // triangle list; nverts is a multiple of 3
    void (*draw)(const XRGpuBatch* b, const XRGpuVert* tri, int nverts);
    // copies a rectangle of the EFB into dst (0xAARRGGBB, `stride` pixels per row); the backend waits for the GPU
    void (*readback)(unsigned* dst, int stride, int x, int y, int w, int h);
    // clear after a copy: colour (rgb / alpha selected), depth; z24 in 0..16777215
    void (*clear)(int rgb, int alpha, unsigned argb, int depth, unsigned z24);
    // GXCopyDisp: the EFB region (0,0,w,h) becomes the picture to show
    void (*display)(int w, int h);
    int noXfb;   // 1: nothing reads the YUV XFB in memory, so the CPU conversion is skipped
} XRGpu;

void XRaster_SetGpu(const XRGpu* gpu);   // NULL = pure software

#ifdef __cplusplus
}
#endif
#endif
