// Archives read through GetDataExt (room, player, enemy, weapon ...): { u32 num; u32 pad[3]; u32 ofs[num]; char tag[num][4]; }
// followed by the blocks the tags name. The disc stores everything big-endian. The header is read big-endian here (it stays as
// it is on the disc); a block is handed to the game only once a converter for its tag exists, because every tag has its own
// layout. A request for a tag without a converter returns NULL and leaves a trace, so the callers that cannot cope with a
// missing block show up one by one instead of reading garbage.
#include "xbox/xdataext.h"
#include "xbox/xswap.h"
#include <string.h>

// Tags whose blocks are converted when they are read (none yet).
static const char* const k_converted[] = {0};

extern "C" unsigned XDataExt_Num(const void* arc)
{
    unsigned n = XSwap_Rd32((const unsigned char*) arc);
    return n <= 256 ? n : 0;
}

extern "C" void* XDataExt_Block(void* arc, unsigned i, const char* tag, void (*trace)(const char*))
{
    const unsigned char* a = (const unsigned char*) arc;
    unsigned n = XDataExt_Num(arc);
    if (i >= n) {
        return 0;
    }
    bool ok = false;
    for (int k = 0; k < (int) (sizeof(k_converted) / sizeof(k_converted[0])) && k_converted[k]; k++) {
        if (strncmp(tag, k_converted[k], 3) == 0) ok = true;
    }
    if (!ok) {
        if (trace) {
            char t[40];
            t[0] = 'D'; t[1] = 'a'; t[2] = 't'; t[3] = 'a'; t[4] = 'E'; t[5] = 'x'; t[6] = 't'; t[7] = ' ';
            for (int k = 0; k < 3 && tag[k]; k++) t[8 + k] = tag[k];
            const char* msg = " not converted\n";
            int p = 8 + 3;
            for (int k = 0; msg[k]; k++) t[p++] = msg[k];
            t[p] = 0;
            trace(t);
        }
        return 0;
    }
    return (void*) (a + XSwap_Rd32(a + 0x10 + i * 4));
}
