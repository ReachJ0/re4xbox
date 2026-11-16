// RE4 for the original Xbox: the game's memory map.
//
// The GameCube game assumes its main memory lives at the cached mirror 0x80000000..0x82FFFFFF and
// hard-codes that in ~70 places (pointer sanity checks, ordering-table tags, the fixed SysMem layout,
// texture addresses). On the Xbox user code cannot be given 0x80000000, so those places go through
// the macros below.
//
// On GameCube (RE4_XBOX undefined) every macro expands to the exact original expression, so the
// matching build is unchanged. On Xbox one block (XMEM_SPAN bytes) plays the role of the GC memory:
// game address A maps to g_xmemBase + (A - 0x80000000), and a pointer is "valid" when it is inside
// that block or inside another registered range (the XBE image, which holds the game's static data
// where the GC kept it inside the same 0x80000000 window).
#ifndef XBOX_XMEM_H
#define XBOX_XMEM_H

#ifdef RE4_XBOX

#ifdef __cplusplus
extern "C" {
#endif

#define XMEM_SPAN 0x01840000u  // 24 MB of GC main memory + the debug buffers the game keeps above it
#define XMEM_MAX_RANGES 4
#define XMEM_MIN_BASE 0x08000000u  // blocks below this could be confused with file offsets
#define XMEM_WANTED_BASE 0x20000000u  // where the backends try to map the block

extern unsigned int g_xmemBase;  // real address that plays the role of 0x80000000

// Declares the block that plays the GC memory. Resets the extra ranges. Returns 0 (and changes
// nothing) when base is below XMEM_MIN_BASE.
int XMem_SetBase(void* base, unsigned int size);
// Registers another region that holds valid game pointers (the XBE image).
int XMem_AddRange(unsigned int lo, unsigned int size);
// Non-zero when p is inside the game block or a registered range.
int XMem_Valid(unsigned int p);
// Non-zero when p is inside the game block (what the GC checks were really about for heap data).
int XMem_InBlock(unsigned int p);

#ifdef __cplusplus
}
#endif

// game address constant (0x80xxxxxx) -> real address
#define XMEM_ADDR(a) ((unsigned int) (g_xmemBase + ((unsigned int) (a) - 0x80000000u)))
#define XMEM_PTR(a) ((void*) (unsigned long) XMEM_ADDR(a))
// offset inside the block -> real address, and back
#define XMEM_FROM_OFF(off) ((unsigned int) (g_xmemBase + (unsigned int) (off)))
#define XMEM_OFF(p) ((unsigned int) ((unsigned int) (p) - g_xmemBase))
#define XMEM_TAGOF(p) XMEM_OFF(p)  // 31-bit ordering-table tag of a pointer
// ordering-table tag (31-bit offset, bit 31 = flag) -> real address
#define XMEM_TAG(v) ((unsigned int) (g_xmemBase + ((unsigned int) (v) & 0x7FFFFFFFu)))
// "is this not a game pointer"
#define XMEM_BAD(p) (!XMem_Valid((unsigned int) (p)))
#define XMEM_OK(p) (XMem_Valid((unsigned int) (p)))
// The GC range checks are written as separate compares (and in several shapes, which MWCC compiles
// differently), so each shape has its own macro; on Xbox the pair collapses to one validity test.
#define XMEM_LT(p) XMEM_BAD(p)       // (u32) p < 0x80000000
#define XMEM_GT(p) (0)               // (u32) p > 0x82FFFFFF
#define XMEM_GE(p) XMEM_OK(p)        // (u32) p >= 0x80000000
#define XMEM_LE(p) (1)               // (u32) p <= 0x82FFFFFF
#define XMEM_SGE0(p) XMEM_BAD(p)     // (s32) p >= 0
#define XMEM_SUB_GT(p) XMEM_BAD(p)   // (u32) p - 0x80000000 > 0x02FFFFFF
#define XMEM_SUB_LE(p) XMEM_OK(p)    // (u32) p - 0x80000000 <= 0x02FFFFFF
// "already a pointer, not a file offset": the GC tests the sign bit ((s32) p < 0) because every
// pointer is 0x8xxxxxxx and every file offset is small. The Xbox block is placed above XMEM_MIN_BASE
// (far above any offset the game files hold) so the same question is "p >= block base".
#define XMEM_ISPTR(p) ((unsigned int) (p) >= g_xmemBase)    // (s32) p < 0
#define XMEM_NOTPTR(p) ((unsigned int) (p) < g_xmemBase)    // (s32) p >= 0

#else  // GameCube: the original expressions

#define XMEM_ADDR(a) (a)
#define XMEM_PTR(a) ((void*) (a))
#define XMEM_FROM_OFF(off) ((off) + 0x80000000)
#define XMEM_OFF(p) ((u32) (p) - 0x80000000)
#define XMEM_TAGOF(p) ((u32) (p) & 0x7FFFFFFF)
#define XMEM_TAG(v) ((v) | 0x80000000)
#define XMEM_BAD(p) ((u32) (p) < 0x80000000 || (u32) (p) > 0x82FFFFFF)
#define XMEM_OK(p) ((u32) (p) >= 0x80000000 && (u32) (p) <= 0x82FFFFFF)
#define XMEM_LT(p) ((u32) (p) < 0x80000000)
#define XMEM_GT(p) ((u32) (p) > 0x82FFFFFF)
#define XMEM_GE(p) ((u32) (p) >= 0x80000000)
#define XMEM_LE(p) ((u32) (p) <= 0x82FFFFFF)
#define XMEM_SGE0(p) ((s32) (p) >= 0)
#define XMEM_SUB_GT(p) ((u32) (p) - 0x80000000 > 0x02FFFFFF)
#define XMEM_SUB_LE(p) ((u32) (p) - 0x80000000 <= 0x02FFFFFF)
#define XMEM_ISPTR(p) ((s32) (p) < 0)
#define XMEM_NOTPTR(p) ((s32) (p) >= 0)

#endif
#endif
