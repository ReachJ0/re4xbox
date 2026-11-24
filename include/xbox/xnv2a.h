// NV2A (original Xbox GPU) backend for the GX rasteriser hook (xrgpu.h), built on pbkit. See src/xbox/xnv2a.cpp.
#ifndef XBOX_XNV2A_H
#define XBOX_XNV2A_H

#include "xbox/xrgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts pbkit and the GPU state. Returns 0 when the GPU path cannot be used (pbkit failed, no contiguous memory);
// the caller then stays on the software rasteriser.
int XNv2a_Init(void);
int XNv2a_Active(void);
const XRGpu* XNv2a_Gpu(void);
// End of a game frame (VI retrace): shows the picture of the last GXCopyDisp with `lines` of text over it, then swaps.
void XNv2a_Present(int black, const char* const* lines, int nlines);
// One line of counters for the HUD.
void XNv2a_StatsLine(int which, char* out, int n);

#ifdef __cplusplus
}
#endif
#endif
