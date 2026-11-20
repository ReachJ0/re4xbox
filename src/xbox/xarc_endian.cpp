// Big-endian -> little-endian conversion of the offset table of an archive (see include/xbox/xarc_endian.h).
#ifdef RE4_XBOX
#include "xbox/xarc_endian.h"
#include "xbox/xswap.h"

extern "C" unsigned XArc_SwapTable(void* arc, unsigned len, unsigned firstSlot)
{
    unsigned char* a = (unsigned char*) arc;

    if (firstSlot >= len / 4) {
        return 0;
    }
    unsigned first = XSwap_Rd32(a + firstSlot * 4);   // offset of the first sub-file = where the table ends
    if (first % 4 != 0 || first <= firstSlot * 4 || first > len) {
        return 0;
    }
    unsigned words = first / 4;
    for (unsigned i = 0; i < words; i++) {
        XSwap_32(a + i * 4);
    }
    return words;
}
#endif
