// Software rasterizer behind XGpBackend (include/xbox/xgp.h): takes the primitive batches the GX command processor
// decodes, applies the XF position matrices / projection / viewport, and draws them into an in-memory EFB
// (640x528, 0xAARRGGBB, plus a float depth buffer). Plain C++ with no platform calls, so the host test can dump the
// result as an image. State it honours: XF matrices, projection, viewport, scissor, cull mode, z mode, blend mode,
// texture sampling (I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 CMPR, level 0, clamp/repeat/mirror, nearest or bilinear),
// the TEV stages (colour/alpha combiners, constants, registers) and the alpha compare.
// GXCopyDisp (BP 0x52) converts the EFB to YUV422 in game memory, with the vertical scale and the copy clear.
// NOT done yet (next steps): lighting (a lit channel shows its material colour), texgen matrices (vertex texture
// coordinates are used as they are), palette (CI) textures, mipmaps, TEV swap tables / compare mode / indirect, fog,
// GXCopyTex.
#ifndef XBOX_XRASTER_H
#define XBOX_XRASTER_H

#include "xbox/xgp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XRASTER_W 640
#define XRASTER_H 528

void XRaster_Bench(unsigned* us);                         // diagnostic: us for 64k pixels of 4 loop kinds (see xraster.cpp)
void XRaster_Slowest(unsigned* out);                      // diagnostic: the slowest draw since the last call (12 words)
void XRaster_SetCapture(const char* path);                // the first draw slower than 0.9 s is written to this file (see tools/xbox/drawcap_replay.cpp)
void XRaster_SetPalettes(const void* slots, int used, unsigned slotSize);   // (replay tool) restores the TLUT slots of a capture
void XRaster_SetFixed(int on);                            // 1 (default): integer pixel pipeline where it applies; 0: always the float pipeline (tests)
void XRaster_Init(void);                                  // allocates the buffers (idempotent) and clears them
const XGpBackend* XRaster_Backend(void);                  // pass to XGp_SetBackend
void XRaster_Clear(unsigned argb, float z24);             // z24: depth in the 24-bit units of the viewport (0..16777215)
const unsigned* XRaster_Pixels(void);                     // XRASTER_W * XRASTER_H, 0xAARRGGBB
const unsigned* XRaster_Depth(void);   // 24 bit depth values
unsigned XRaster_CopySerial(void);                        // counts EFB -> XFB copies (GXCopyDisp), so a presenter knows when the XFB changed
void XRaster_SetClock(unsigned long long (*us)(void));    // optional microsecond clock for the statistics below
void XRaster_Stats(unsigned long long* usDraw, unsigned long long* usCopy, unsigned long long* px, unsigned long long* tris,
                   unsigned long long* draws);       // totals since the start: draw / copy time, pixels written, triangles, draw calls
void XRaster_BigDraws(unsigned long long* maxPx, unsigned long long* bigDraws);   // largest draw call and draws > 20000 px since the last call
unsigned long long XRaster_Triangles(void);               // triangles that reached the pixel stage (statistics)

#ifdef __cplusplus
}
#endif
#endif
