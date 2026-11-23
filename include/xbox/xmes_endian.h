// xbox/xmes_endian.h: the game's message files (core.das messages, the sub screen / card messages, room and event
// text) converted in place from big-endian. Layout (MessageData::getAddr): u32 table[n] where table[1 + language] is
// the offset of that language's block; a block is { u32 x0; u32 count; u32 ofs[count]; ... } with each ofs relative to the
// block, and the text is a stream of u16 (characters, control codes and their arguments are all u16).
#ifndef XBOX_XMES_ENDIAN_H
#define XBOX_XMES_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

// Converts the message file `data` (`len` bytes) in place: the table and each block's count / offsets as u32, the
// text as u16. Returns the number of language blocks converted, 0 if the layout does not fit in `len` (then nothing
// is touched).
int XMes_Swap(void* data, unsigned len);

// First rejected file: [0] reason (1 too short, 2 bad first offset, 3 too many languages, 4 bad block offset, 5 bad
// count), [1] length, [2..5] its first four words. Shown by the development heartbeat; all zero if nothing failed.
extern unsigned XMes_Diag[6];
extern unsigned XMes_Patched;   // phrases rewritten ("Memory Card" -> "Disc Drive") while converting
extern unsigned short XMes_Sample[16];   // the first 16 text words of the first converted block (diagnostic)

#ifdef __cplusplus
}
#endif
#endif
