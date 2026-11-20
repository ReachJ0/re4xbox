// Big-endian -> little-endian conversion of the GameCube sound files (see include/xbox/xsnd_endian.h).
#ifdef RE4_XBOX
#include "xbox/xsnd_endian.h"
#include "xbox/xswap.h"

namespace {

inline unsigned Sw32(unsigned char* p) { return XSwap_32(p); }
inline unsigned Sw16(unsigned char* p) { return XSwap_16(p); }

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

namespace {
inline bool Fits(unsigned ofs, unsigned size, unsigned len) { return ofs <= len && size <= len - ofs; }
}  // namespace

extern "C" int XSnd_SwapU32Array(void* data, unsigned len)
{
    unsigned char* d = (unsigned char*) data;
    for (unsigned i = 0; i + 4 <= len; i += 4) {
        Sw32(d + i);
    }
    return len >= 4;
}

extern "C" int XSnd_SwapDoorTbl(void* data, unsigned len)
{
    unsigned char* d = (unsigned char*) data;
    if (len < 8) {
        return 0;
    }
    unsigned fileOfs = Sw32(d), numOfs = Sw32(d + 4);
    if (!Fits(numOfs, 2, len)) {
        return 0;
    }
    unsigned cnt = Sw16(d + numOfs);
    if (!Fits(fileOfs, cnt * 4, len)) {
        return 0;
    }
    for (unsigned i = 0; i < cnt; i++) {
        Sw32(d + fileOfs + i * 4);
    }
    return 1;
}

extern "C" int XSnd_SwapBgmTbl(void* data, unsigned len)
{
    unsigned char* d = (unsigned char*) data;
    int ok = 1;
    if (len < 8) {
        return 0;
    }
    unsigned roomOfs = Sw32(d), listOfs = Sw32(d + 4);
    unsigned rooms = 0;
    if (listOfs > len) {
        return 0;
    }
    // the id list: u16 until 0xFFFF (inclusive)
    for (unsigned p = listOfs; p + 2 <= len; p += 2, rooms++) {
        if (Sw16(d + p) == 0xFFFF) {
            break;
        }
    }
    if (!Fits(roomOfs, rooms * 4, len)) {
        return 0;
    }
    for (unsigned i = 0; i < rooms; i++) {
        unsigned rel = Sw32(d + roomOfs + i * 4);
        unsigned at = roomOfs + rel;
        bool shared = false;   // two rooms may point at the same block: convert it once
        for (unsigned j = 0; j < i; j++) {
            shared = shared || *(unsigned*) (d + roomOfs + j * 4) == rel;
        }
        if (shared) {
            continue;
        }
        if (rel > len || !Fits(at, 4, len)) {
            ok = 0;
            continue;
        }
        unsigned num = Sw32(d + at);
        if (num > 64 || !Fits(at + 4, num * 13 * 4, len)) {
            ok = 0;
            continue;
        }
        for (unsigned k = 0; k < num * 13; k++) {
            Sw32(d + at + 4 + k * 4);
        }
    }
    return ok;
}
#endif
