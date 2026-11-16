// Host test for the Xbox memory map (include/xbox/xmem.h, src/xbox/xmem_xbox.cpp).
#include <stdio.h>
#include <stdint.h>
#include "xbox/xmem.h"

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef unsigned int u32;
typedef int s32;
// the GameCube expressions, as the game had them
static bool gcBad(u32 p) { return p < 0x80000000u || p > 0x82FFFFFFu; }
static bool gcSubGt(u32 p) { return p - 0x80000000u > 0x02FFFFFFu; }
static bool gcIsPtr(u32 p) { return (s32) p < 0; }

int main()
{
    CHECK(!XMem_SetBase((void*) 0x00400000, XMEM_SPAN));            // too low: would be confused with offsets
    CHECK(!XMem_SetBase((void*) (uintptr_t) XMEM_WANTED_BASE, 0));
    CHECK(g_xmemBase == 0x80000000u);                               // refused calls change nothing
    const u32 B = XMEM_WANTED_BASE;
    CHECK(XMem_SetBase((void*) (uintptr_t) B, XMEM_SPAN) && g_xmemBase == B);

    // addresses: game constant <-> real
    CHECK(XMEM_ADDR(0x80350000u) == B + 0x350000u && XMEM_ADDR(0x80000000u) == B);
    CHECK(XMEM_OFF(B + 0x974000u) == 0x974000u && XMEM_FROM_OFF(0x974000u) == B + 0x974000u);
    CHECK((u32) (uintptr_t) XMEM_PTR(0x8181FB00u) == B + 0x181FB00u);

    // validity: for every 4 KB step over the whole GC window, the Xbox answer on the mapped address is the GC
    // answer on the original, except above the block (GC allowed 48 MB, the block holds 24.25 MB)
    int mism = 0;
    for (u32 off = 0; off < 0x03000000u; off += 0x1000) {
        u32 gc = 0x80000000u + off, xb = B + off;
        bool expectBad = off >= XMEM_SPAN;
        if (XMEM_BAD(xb) != expectBad || XMEM_OK(xb) == expectBad) mism++;
        if (!expectBad && (XMEM_BAD(xb) != gcBad(gc) || XMEM_SUB_GT(xb) != gcSubGt(gc))) mism++;
        if (XMEM_LT(xb) != expectBad || XMEM_GT(xb) || !XMEM_LE(xb) || XMEM_GE(xb) == expectBad) mism++;
        if (XMEM_SUB_LE(xb) == expectBad) mism++;
    }
    CHECK(mism == 0);
    // edges
    CHECK(!XMEM_BAD(B) && !XMEM_BAD(B + XMEM_SPAN - 1) && XMEM_BAD(B + XMEM_SPAN) && XMEM_BAD(B - 1));
    CHECK(XMEM_BAD(0u) && XMEM_BAD(0x1234u) && XMEM_BAD(0xFFFFFFFFu) && XMEM_BAD(0x80000000u));
    // small ints and file offsets are never pointers; every pointer of the block is
    CHECK(XMEM_NOTPTR(0u) && XMEM_NOTPTR(0x02FFFFFFu) && XMEM_NOTPTR(B - 1) && XMEM_ISPTR(B) && XMEM_ISPTR(B + 0x01000000u));
    for (u32 off = 0; off < XMEM_SPAN; off += 0x10000) CHECK(XMEM_ISPTR(B + off) && gcIsPtr(0x80000000u + off));
    // ordering-table tags: pointer -> tag (31 bit) -> pointer, with and without the flag bit the game sets
    for (u32 off = 0; off < XMEM_SPAN; off += 0x1F30) {
        u32 p = B + off, tag = XMEM_TAGOF(p);
        CHECK(tag == off && XMEM_TAG(tag) == p && XMEM_TAG(tag | 0x80000000u) == p);
    }
    CHECK(XMEM_TAG(0x7FFFFFFFu) == B + 0x7FFFFFFFu);                 // wraps like any offset; callers keep tags < SPAN

    // extra ranges (the XBE image)
    CHECK(XMEM_BAD(0x00011000u));
    CHECK(XMem_AddRange(0x00010000u, 0x00200000u));
    CHECK(!XMEM_BAD(0x00010000u) && !XMEM_BAD(0x001FFFFFu) && XMEM_BAD(0x00210000u) && XMEM_BAD(0x0000FFFFu));
    CHECK(XMEM_NOTPTR(0x00011000u));                                  // static data is valid but not "a block pointer"
    CHECK(!XMem_AddRange(0x100u, 0) && !XMem_AddRange(0xFFFFFFF0u, 0x100u));
    for (int i = 1; i < XMEM_MAX_RANGES; i++) CHECK(XMem_AddRange(0x30000000u + i * 0x1000u, 0x100u));
    CHECK(!XMem_AddRange(0x40000000u, 0x100u));                       // table full
    CHECK(!XMEM_BAD(0x30001000u) && !XMEM_BAD(0x30003000u) && XMEM_BAD(0x30004000u));
    CHECK(XMem_InBlock(B) && !XMem_InBlock(0x00011000u));
    // moving the block forgets the old ranges
    CHECK(XMem_SetBase((void*) 0x30000000u, XMEM_SPAN) && XMEM_BAD(0x00011000u) && !XMEM_BAD(0x30000000u) && XMEM_BAD(B));

    printf("%d checks, %d failures\n", g_n, g_bad);
    return g_bad != 0;
}
