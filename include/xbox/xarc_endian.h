// xbox/xarc_endian.h: the offset table at the front of the game's archives (core, player, room...) is a run of
// u32 words, big-endian on the disc; the game indexes it as ((u32*) arc)[no] (CORE_ARC, PL_ARC...).
#ifndef XBOX_XARC_ENDIAN_H
#define XBOX_XARC_ENDIAN_H

#include "xbox/xtex_endian.h"
#include "xbox/xmes_endian.h"

#ifdef __cplusplus
extern "C" {
#endif

// Converts the table of `arc` in place: every u32 word before the first sub-file, whose offset is word
// `firstSlot` (4 for the core archive: four header words, then the offsets). `len` is the size of the buffer.
// Returns the number of words converted, 0 if the table does not fit (nothing is touched then).
unsigned XArc_SwapTable(void* arc, unsigned len, unsigned firstSlot);

#ifdef __cplusplus
}
#endif

// End of the sub-file that starts at `start` in an archive whose (already converted) table has `words` entries: the
// next larger offset in the table, or `len`.
static inline unsigned XArc_SubEnd(const unsigned* table, unsigned words, unsigned start, unsigned len)
{
    unsigned end = len, i;
    for (i = 0; i < words; i++) {
        if (table[i] > start && table[i] < end) {
            end = table[i];
        }
    }
    return end;
}

// An archive laid out like CardArc (card.cpp): four header words, six offsets, the first sub-file being a TPL (the
// card icons / banner in SndMem.sub_adr, the second the message table). Converts the table, the TPL and the
// messages; returns 1 on success.
static inline int XArc_SwapCardArc(void* arc, unsigned len)
{
    unsigned* o = (unsigned*) arc + 4;
    unsigned end;
    if (!XArc_SwapTable(arc, len, 4)) {
        return 0;
    }
    end = XArc_SubEnd(o, 6, o[0], len);
    if (!XTex_SwapTpl((unsigned char*) arc + o[0], end - o[0])) {
        return 0;
    }
    if (o[1] < len) {   // the message table: sub-file 1
        return XMes_Swap((unsigned char*) arc + o[1], XArc_SubEnd(o, 6, o[1], len) - o[1]) != 0;
    }
    return 1;
}

#endif
