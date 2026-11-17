// Cache and locked-cache calls on the Xbox. The x86 caches are coherent with the GPU's DMA for the buffers the
// game flushes, so the data/instruction cache calls do nothing (DCZeroRange still has to clear the memory). The
// GameCube's locked cache (16 KB at 0xE0000000, used by the game as scratch memory and as the skinning matrix
// palette) is a plain 16 KB buffer; the LC DMA calls are copies.
#ifdef RE4_XBOX

#include <dolphin/os.h>
#include <string.h>

#include "xbox/xmem.h"

static unsigned char s_lc[XMEM_LC_SIZE] __attribute__((aligned(32)));

extern "C" {

void* XMem_LockedCache(void) { return s_lc; }

void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCTouchRange(void*, u32) {}
void ICInvalidateRange(void*, u32) {}
void DCZeroRange(void* addr, u32 nBytes) { memset(addr, 0, nBytes); }

void LCEnable(void) {}
void LCDisable(void) {}
void LCLoadBlocks(void* destTag, void* srcAddr, u32 numBlocks) { memcpy(destTag, srcAddr, numBlocks * 32); }
void LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks) { memcpy(destAddr, srcTag, numBlocks * 32); }
u32 LCLoadData(void* destAddr, void* srcAddr, u32 nBytes)
{
    memcpy(destAddr, srcAddr, nBytes);
    return 0;  // blocks queued: the copy is already done
}
u32 LCStoreData(void* destAddr, void* srcAddr, u32 nBytes)
{
    memcpy(destAddr, srcAddr, nBytes);
    return 0;
}
u32 LCQueueLength(void) { return 0; }
void LCQueueWait(u32) {}
void LCFlushQueue(void) {}
void __OSCacheInit(void) {}

}  // extern "C"
#endif
