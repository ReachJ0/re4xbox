// xbox/xswap.h: in-place big-endian -> little-endian helpers for the conversion of GameCube disc data.
// The bytes at p are big-endian before the call and native (little-endian) after; the value is returned.
#ifndef XBOX_XSWAP_H
#define XBOX_XSWAP_H

static inline unsigned XSwap_Rd32(const unsigned char* p)
{
    return (unsigned) p[0] << 24 | (unsigned) p[1] << 16 | (unsigned) p[2] << 8 | p[3];
}

static inline unsigned XSwap_32(unsigned char* p)
{
    unsigned v = XSwap_Rd32(p);
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
    return v;
}

static inline unsigned XSwap_16(unsigned char* p)
{
    unsigned char t = p[0];
    p[0] = p[1];
    p[1] = t;
    return (unsigned) t << 8 | p[0];   // the value that was stored big-endian
}

#endif
