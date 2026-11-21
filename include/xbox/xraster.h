// Software rasterizer behind XGpBackend (include/xbox/xgp.h): takes the primitive batches the GX command processor
// decodes, applies the XF position matrices / projection / viewport, and draws them into an in-memory EFB
// (640x528, 0xAARRGGBB, plus a float depth buffer). Plain C++ with no platform calls, so the host test can dump the
// result as an image. State it honours: XF matrices, projection, viewport, scissor, cull mode, z mode, blend mode.
// NOT done yet (next steps): lighting / material channels, texture sampling, TEV, alpha compare, fog, EFB copies.
#ifndef XBOX_XRASTER_H
#define XBOX_XRASTER_H

#include "xbox/xgp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XRASTER_W 640
#define XRASTER_H 528

void XRaster_Init(void);                                  // allocates the buffers (idempotent) and clears them
const XGpBackend* XRaster_Backend(void);                  // pass to XGp_SetBackend
void XRaster_Clear(unsigned argb, float z24);             // z24: depth in the 24-bit units of the viewport (0..16777215)
const unsigned* XRaster_Pixels(void);                     // XRASTER_W * XRASTER_H, 0xAARRGGBB
const float* XRaster_Depth(void);
unsigned long long XRaster_Triangles(void);               // triangles that reached the pixel stage (statistics)

#ifdef __cplusplus
}
#endif
#endif
