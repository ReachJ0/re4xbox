// Big-endian -> little-endian conversion of the GameCube sound files (see include/xbox/xsnd_endian.h).
#ifdef RE4_XBOX
#include "xbox/xsnd_endian.h"

namespace {

inline unsigned Rd32(const unsigned char* p) { return (unsigned) p[0] << 24 | (unsigned) p[1] << 16 | (unsigned) p[2] << 8 | p[3]; }

// swaps the u32 at p in place (the bytes are big-endian before; little-endian after)
inline unsigned Sw32(unsigned char* p)
{
    unsigned v = Rd32(p);
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
    return v;
}

inline unsigned Sw16(unsigned char* p)
{
    unsigned char t = p[0];
    p[0] = p[1];
    p[1] = t;
    return (unsigned) p[0] | (unsigned) t << 8;   // value after the swap
}

const unsigned kRitSize = 0x10, kShdSize = 0x7C;

}  // namespace

extern "C" int XSnd_SwapStrHed(void* data, unsigned len)
{
    unsigned char* d = (unsigned char*) data;
    int ok = 1;

    if (len < 8) {
        return 0;
    }
    unsigned blkOfs[2];
    blkOfs[0] = Sw32(d);
    blkOfs[1] = Sw32(d + 4);
    for (int b = 0; b < 2; b++) {
        unsigned base = blkOfs[b];
        if (base > len || len - base < 12) {
            ok = 0;
            continue;
        }
        unsigned char* h = d + base;
        unsigned ritNum = Sw32(h), sbhOfs = Sw32(h + 4), ritOfs = Sw32(h + 8);
        unsigned maxStr = 0;
        int haveRit = 0;

        if (ritOfs <= len - base && ritNum <= (len - base - ritOfs) / kRitSize) {
            unsigned char* r = h + ritOfs;
            for (unsigned i = 0; i < ritNum; i++, r += kRitSize) {
                unsigned s = Sw16(r);   // str_no (s16)
                Sw16(r + 0x0C);         // flag
                if ((s & 0x8000) == 0 && s + 1 > maxStr) {
                    maxStr = s + 1;
                }
            }
            haveRit = 1;
        } else {
            ok = 0;
        }
        if (haveRit && sbhOfs <= len - base && maxStr <= (len - base - sbhOfs) / 4) {
            unsigned char* t = h + sbhOfs;
            for (unsigned i = 0; i < maxStr; i++) {
                unsigned off = Sw32(t + i * 4);
                if (off <= len - base - sbhOfs && kShdSize <= len - base - sbhOfs - off) {
                    unsigned char* s = t + off;
                    for (int k = 0; k < 8; k++) {
                        Sw32(s + k * 4);
                    }
                    for (int k = 0; k < 46; k++) {
                        Sw16(s + 0x20 + k * 2);
                    }
                } else {
                    ok = 0;
                }
            }
        } else {
            ok = 0;
        }
    }
    return ok;
}
#endif
