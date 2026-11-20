// xbox/xtex_endian.h: GameCube texture palettes (TPL) and the font files that carry one, converted in place
// from big-endian. Only the headers are converted: the pixel data stays in the GameCube's own tiled formats
// (the renderer decodes those; see docs/xbox.md).
#ifndef XBOX_XTEX_ENDIAN_H
#define XBOX_XTEX_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

// A TPL as loaded from disc: TEXPalette { version, numDescriptors, descriptorArray offset } at `tpl`,
// descriptors { TEXHeader offset, CLUTHeader offset } and the two headers, all offsets relative to `tpl`.
// `len` is the number of bytes from `tpl` to the end of the buffer. Returns 1 if everything was converted,
// 0 if some offset pointed outside the buffer (that part is left alone).
int XTex_SwapTpl(void* tpl, unsigned len);

// A .fnt file (Font/*.fnt): u32 offset of the TPL, u32 offset of the width table, then those. Converts the two
// offsets and the TPL. Returns like XTex_SwapTpl.
int XTex_SwapFnt(void* data, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
