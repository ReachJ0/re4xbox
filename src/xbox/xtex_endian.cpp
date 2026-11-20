// Big-endian -> little-endian conversion of GameCube texture palettes (see include/xbox/xtex_endian.h).
#ifdef RE4_XBOX
#include "xbox/xtex_endian.h"
#include "xbox/xswap.h"

namespace {

const unsigned kDescSize = 8, kTexHdrSize = 0x24, kClutHdrSize = 0xC, kMaxDesc = 4096;

bool Fits(unsigned ofs, unsigned size, unsigned len) { return ofs <= len && size <= len - ofs; }

}  // namespace

extern "C" int XTex_SwapTpl(void* tpl, unsigned len)
{
    unsigned char* b = (unsigned char*) tpl;
    int ok = 1;

    if (len < 12) {
        return 0;
    }
    XSwap_32(b);   // version
    unsigned n = XSwap_32(b + 4), dOfs = XSwap_32(b + 8);
    if (n > kMaxDesc || !Fits(dOfs, n * kDescSize, len)) {
        return 0;
    }
    unsigned char* d = b + dOfs;
    for (unsigned i = 0; i < n; i++, d += kDescSize) {
        unsigned th = XSwap_32(d), ch = XSwap_32(d + 4);
        // two descriptors may share a header: convert each header once
        bool thSeen = false, chSeen = false;
        for (unsigned j = 0; j < i; j++) {
            unsigned pth = *(unsigned*) (b + dOfs + j * kDescSize), pch = *(unsigned*) (b + dOfs + j * kDescSize + 4);
            thSeen = thSeen || pth == th;
            chSeen = chSeen || pch == ch;
        }
        if (!thSeen) {
            if (Fits(th, kTexHdrSize, len)) {
                unsigned char* h = b + th;
                XSwap_16(h);
                XSwap_16(h + 2);
                for (int k = 4; k < 0x20; k += 4) {   // format, data, wrapS/T, minFilter, magFilter, LODBias
                    XSwap_32(h + k);
                }                                      // 0x20..0x23: edgeLODEnable, minLOD, maxLOD, unpacked
            } else {
                ok = 0;
            }
        }
        if (!chSeen && ch != 0) {   // 0: no CLUT (the game then points at the start of the TPL and never reads it)
            if (Fits(ch, kClutHdrSize, len)) {
                unsigned char* h = b + ch;
                XSwap_16(h);   // numEntries; unpacked / pad are bytes
                XSwap_32(h + 4);
                XSwap_32(h + 8);
            } else {
                ok = 0;
            }
        }
    }
    return ok;
}

extern "C" int XTex_SwapFnt(void* data, unsigned len)
{
    unsigned char* b = (unsigned char*) data;

    if (len < 8) {
        return 0;
    }
    unsigned tpl = XSwap_32(b);
    XSwap_32(b + 4);   // width table offset (u8 entries, nothing else to convert)
    if (tpl > len) {
        return 0;
    }
    return XTex_SwapTpl(b + tpl, len - tpl);
}
#endif
