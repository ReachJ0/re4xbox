// PowerPC compiler intrinsics the SDK uses (CodeWarrior builtins), for the Xbox build.
#ifndef XBOX_XPPC_H
#define XBOX_XPPC_H
#ifdef RE4_XBOX
// PowerPC mask: bits mb..me counted from the most significant bit (wraps when mb > me)
static inline unsigned int xppc_mask(int mb, int me)
{
    mb &= 31;  // the instruction encodes 5-bit fields
    me &= 31;
    unsigned int a = 0xFFFFFFFFu >> mb;
    unsigned int b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
static inline unsigned int xppc_rotl(unsigned int v, int sh)
{
    sh &= 31;
    return sh ? (v << sh) | (v >> (32 - sh)) : v;
}
// rlwimi: insert the rotated source into `a` under the mask
static inline unsigned int __rlwimi(unsigned int a, unsigned int b, int sh, int mb, int me)
{
    unsigned int m = xppc_mask(mb, me);
    return (xppc_rotl(b, sh) & m) | (a & ~m);
}
static inline unsigned int __rlwinm(unsigned int v, int sh, int mb, int me)
{
    return xppc_rotl(v, sh) & xppc_mask(mb, me);
}
#endif
#endif
