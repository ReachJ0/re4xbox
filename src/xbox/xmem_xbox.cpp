// Memory map of the Xbox port (see include/xbox/xmem.h).
#ifdef RE4_XBOX
#include "xbox/xmem.h"

unsigned int g_xmemBase = 0x80000000u;  // identity until the OS layer allocates the block

static unsigned int s_size = XMEM_SPAN;
static unsigned int s_lo[XMEM_MAX_RANGES];
static unsigned int s_len[XMEM_MAX_RANGES];
static int s_n;

int XMem_SetBase(void* base, unsigned int size)
{
    if ((unsigned int) (unsigned long) base < XMEM_MIN_BASE || size == 0) {
        return 0;
    }
    g_xmemBase = (unsigned int) (unsigned long) base;
    s_size = size;
    s_n = 0;
    return 1;
}

int XMem_AddRange(unsigned int lo, unsigned int size)
{
    if (size == 0 || s_n >= XMEM_MAX_RANGES || lo + size < lo) {
        return 0;
    }
    s_lo[s_n] = lo;
    s_len[s_n] = size;
    s_n++;
    return 1;
}

int XMem_InBlock(unsigned int p) { return p - g_xmemBase < s_size; }

int XMem_Valid(unsigned int p)
{
    int i;
    if (p - g_xmemBase < s_size) {
        return 1;
    }
    for (i = 0; i < s_n; i++) {
        if (p - s_lo[i] < s_len[i]) {
            return 1;
        }
    }
    return 0;
}
#endif
