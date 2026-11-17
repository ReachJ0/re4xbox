// C replacements for the hand-written PowerPC units of the game (game/memset_2.cpp, game/yz2asm.cpp),
// which are whole-function asm() bodies and are not part of the Xbox build.
#include <string.h>
#include <stdint.h>
#include <dolphin/os.h>

extern "C" {

// memset_2.cpp: void memset_asm(void* dst, int c, u32 n) -- the original fills words in 16-byte blocks
// when dst is word aligned and bytes otherwise; the result is the same as memset.
void memset_asm(void* dst, int c, uint32_t n) { memset(dst, c & 0xFF, n); }
void memclr_asm(void* dst, uint32_t n) { memset(dst, 0, n); }
// zero `count` 3x4 float matrices (48 bytes each)
void mtxclr_asm(void* dst, uint32_t count) { memset(dst, 0, (size_t) count * 48); }

// yz2asm.cpp: the yz2 / FrequencyDecode decompressor is ~500 lines of PowerPC assembly and has NOT been
// ported yet. Until it is, decoding fails loudly instead of returning garbage.
void yz2Decode_Decode(void*, void*, uint32_t, void*) { OSPanic("yz2asm", 0, "yz2Decode_Decode: not ported to x86 yet"); }

}
