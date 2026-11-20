// xbox/xarc_endian.h: the offset table at the front of the game's archives (core, player, room...) is a run of
// u32 words, big-endian on the disc; the game indexes it as ((u32*) arc)[no] (CORE_ARC, PL_ARC...).
#ifndef XBOX_XARC_ENDIAN_H
#define XBOX_XARC_ENDIAN_H

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

#endif
