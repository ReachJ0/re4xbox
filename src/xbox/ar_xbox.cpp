// xbox/ar_xbox: Xbox replacement for the Dolphin SDK ARAM libraries (src/lib/ar.c, arq.c).
//
// Provides the calls the game makes, plus the few that cost nothing:
//   ARInit ARAlloc ARFree ARCheckInit ARGetBaseAddress ARGetSize ARGetInternalSize
//   ARQInit ARQReset ARQPostRequest ARQRemoveRequest ARQRemoveOwnerRequest ARQFlushQueue
//   ARQSetChunkSize ARQGetChunkSize ARQCheckInit
// Not provided (the game does not call them): ARStartDMA, ARRegisterDMACallback, ARGetDMAStatus, ARReset,
// ARSetSize, ARClear.
//
// ARAM is a 16 MB buffer in main memory (allocated on first use). An ARQ request is a copy between it and
// main memory, queued like the SDK queues them (high priority list first, first come first served in a
// list) and carried out, with its callback, by XDvd_Pump (xbox/xdvd.h), never inside ARQPostRequest: the
// game sets "busy" flags right after posting, and the callback clears them. Main memory addresses are
// plain pointers, since on the Xbox they fit in the game's 32-bit u32 (the game does `(u32) buf`).
//
// Differences from the real library: a request outside ARAM is refused (counted in XDvd_ErrorCount, its
// callback still runs so nothing waits forever) where the SDK would copy garbage or stop in OSPanic;
// ARAlloc / ARFree on bad arguments return 0 and count instead of OSPanic.
//
// Only compiled for the Xbox build (RE4_XBOX).
#ifdef RE4_XBOX

#include <dolphin/ar.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xbox/xdvd.h"

#define ARAM_USER_BASE 0x4000u  // the first 16 KB belong to the DSP

namespace {

unsigned char* s_aram = 0;
int s_arInit = 0;
u32 s_stackPtr = ARAM_USER_BASE;
u32 s_freeBlocks = 0;
u32* s_blockLen = 0;

int s_arqInit = 0;
u32 s_chunk = 4096;
ARQRequest* s_hiHead = 0;
ARQRequest* s_hiTail = 0;
ARQRequest* s_loHead = 0;
ARQRequest* s_loTail = 0;

unsigned char* Aram()
{
    if (!s_aram) {
        s_aram = (unsigned char*) calloc(XARAM_SIZE, 1);
    }
    return s_aram;
}

void Push(ARQRequest*& head, ARQRequest*& tail, ARQRequest* r)
{
    r->next = 0;
    if (tail) {
        tail->next = r;
    } else {
        head = r;
    }
    tail = r;
}

// removes every request for which `match` says yes
template <typename F> void RemoveIf(ARQRequest*& head, ARQRequest*& tail, F match)
{
    ARQRequest* keepHead = 0;
    ARQRequest* keepTail = 0;
    ARQRequest* r = head;

    while (r) {
        ARQRequest* next = r->next;
        if (!match(r)) {
            Push(keepHead, keepTail, r);
        }
        r = next;
    }
    head = keepHead;
    tail = keepTail;
}

void Transfer(const ARQRequest* r)
{
    unsigned char* aram = Aram();
    u32 aramAddr = r->type == ARQ_TYPE_MRAM_TO_ARAM ? r->dest : r->source;
    unsigned char* mram = (unsigned char*) (uintptr_t) (r->type == ARQ_TYPE_MRAM_TO_ARAM ? r->source : r->dest);

    if (!aram || (r->type != ARQ_TYPE_MRAM_TO_ARAM && r->type != ARQ_TYPE_ARAM_TO_MRAM) || r->length > XARAM_SIZE ||
        aramAddr > XARAM_SIZE - r->length || (r->length > 0 && !mram)) {
        XDvd_CountError();
        return;
    }
    if (r->type == ARQ_TYPE_MRAM_TO_ARAM) {
        memcpy(aram + aramAddr, mram, r->length);
    } else {
        memcpy(mram, aram + aramAddr, r->length);
    }
}

}  // namespace

extern "C" {

// ---- the layer's own entry points (xbox/xdvd.h) --------------------------------------------------------

unsigned char* XAram_Memory(void) { return Aram(); }

int XAram_Pending(void) { return s_hiHead != 0 || s_loHead != 0; }

// One request: taken off its list first, so its callback may post the same request again.
int XAram_Service(void)
{
    ARQRequest* r;

    if (s_hiHead) {
        r = s_hiHead;
        s_hiHead = r->next;
        if (!s_hiHead) {
            s_hiTail = 0;
        }
    } else if (s_loHead) {
        r = s_loHead;
        s_loHead = r->next;
        if (!s_loHead) {
            s_loTail = 0;
        }
    } else {
        return 0;
    }
    r->next = 0;
    Transfer(r);
    if (r->callback) {
        r->callback((u32) (uintptr_t) r);
    }
    return 1;
}

void XAram_Reset(void)
{
    free(s_aram);
    s_aram = 0;
    s_arInit = 0;
    s_stackPtr = ARAM_USER_BASE;
    s_freeBlocks = 0;
    s_blockLen = 0;
    s_arqInit = 0;
    s_chunk = 4096;
    s_hiHead = s_hiTail = s_loHead = s_loTail = 0;
}

// ---- AR ------------------------------------------------------------------------------------------------

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    if (s_arInit) {
        return ARAM_USER_BASE;
    }
    Aram();
    s_stackPtr = ARAM_USER_BASE;
    s_freeBlocks = num_entries;
    s_blockLen = stack_index_addr;
    s_arInit = 1;
    return s_stackPtr;
}

BOOL ARCheckInit(void) { return s_arInit ? TRUE : FALSE; }

u32 ARAlloc(u32 length)
{
    u32 addr;

    if ((length & 0x1F) || length > XARAM_SIZE - s_stackPtr || s_freeBlocks == 0 || !s_blockLen) {
        XDvd_CountError();
        return 0;
    }
    addr = s_stackPtr;
    s_stackPtr += length;
    *s_blockLen = length;
    s_blockLen += 1;
    s_freeBlocks -= 1;
    return addr;
}

u32 ARFree(u32* length)
{
    if (!s_blockLen) {
        XDvd_CountError();
        return 0;
    }
    s_blockLen -= 1;
    if (length) {
        *length = *s_blockLen;
    }
    s_stackPtr -= *s_blockLen;
    s_freeBlocks += 1;
    return s_stackPtr;
}

u32 ARGetBaseAddress(void) { return ARAM_USER_BASE; }

u32 ARGetSize(void) { return XARAM_SIZE; }

u32 ARGetInternalSize(void) { return XARAM_SIZE; }

// ---- ARQ -----------------------------------------------------------------------------------------------

void ARQInit(void)
{
    if (s_arqInit) {
        return;
    }
    s_chunk = 4096;
    s_hiHead = s_hiTail = s_loHead = s_loTail = 0;
    s_arqInit = 1;
}

void ARQReset(void)
{
    s_arqInit = 0;
    s_hiHead = s_hiTail = s_loHead = s_loTail = 0;
}

BOOL ARQCheckInit(void) { return s_arqInit ? TRUE : FALSE; }

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback)
{
    if (!request || (length % ARQ_DMA_ALIGNMENT) != 0 || (type != ARQ_TYPE_MRAM_TO_ARAM && type != ARQ_TYPE_ARAM_TO_MRAM) ||
        (priority != ARQ_PRIORITY_LOW && priority != ARQ_PRIORITY_HIGH)) {
        XDvd_CountError();  // asserts in the SDK
        if (request && request->callback == 0 && callback) {
            // nothing queued, so nothing will ever call back: the caller is told by the error count only
        }
        return;
    }
    request->next = 0;
    request->owner = owner;
    request->type = type;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (priority == ARQ_PRIORITY_HIGH) {
        Push(s_hiHead, s_hiTail, request);
    } else {
        Push(s_loHead, s_loTail, request);
    }
}

void ARQRemoveRequest(ARQRequest* request)
{
    RemoveIf(s_hiHead, s_hiTail, [request](ARQRequest* r) { return r == request; });
    RemoveIf(s_loHead, s_loTail, [request](ARQRequest* r) { return r == request; });
}

void ARQRemoveOwnerRequest(u32 owner)
{
    RemoveIf(s_hiHead, s_hiTail, [owner](ARQRequest* r) { return r->owner == owner; });
    RemoveIf(s_loHead, s_loTail, [owner](ARQRequest* r) { return r->owner == owner; });
}

void ARQFlushQueue(void) { s_hiHead = s_hiTail = s_loHead = s_loTail = 0; }

void ARQSetChunkSize(u32 size) { s_chunk = (size + 31) & ~31u; }

u32 ARQGetChunkSize(void) { return s_chunk; }

}  // extern "C"

#endif  // RE4_XBOX
