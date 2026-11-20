// xbox/xsnd_endian.h: the GameCube disc files are big-endian, the Xbox CPU is little-endian.
// The sound files keep offsets and sizes the game reads as plain u32 / u16, so each one has to be converted
// in place once, right after it is read. One function per file format; they never touch a file twice
// as long as the game reads each file once.
#ifndef XBOX_XSND_ENDIAN_H
#define XBOX_XSND_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

// bgm/bio4str.hed (DVD file 0x11 / entry 0): table of 2 block offsets; each block is
// { u32 rit_num, u32 sbh_off, u32 rit_off } (block-relative offsets), RIT[rit_num] (0x10 bytes: s16 str_no at 0,
// u16 flag at 0x0C), a u32 table of offsets to the stream headers (SND_SHD, 0x7C bytes: 8 u32 then 46 u16).
// `len` is the file size; anything that points outside it is left alone (and the function returns 0).
// Returns 1 if every part was converted.
int XSnd_SwapStrHed(void* data, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
