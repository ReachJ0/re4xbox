// xbox/xidtex_endian.h: big-endian -> native conversion of the "id" data files (title.dat, omk_tX.dat ...):
// an archive { u32 ofs[16] } whose blocks are IdTexData (texture pack) or ID_FILE_HEADER (screen widget table).
#ifndef XBOX_XIDTEX_ENDIAN_H
#define XBOX_XIDTEX_ENDIAN_H
#ifdef __cplusplus
extern "C" {
#endif

// IdTexData (version 0xB): header, id table, TPL/TexAnm offset tables, the TPLs and the leading fields of each TexAnm.
// `len` = bytes available from `data`. Returns 1 if everything was converted.
int XIdTex_Swap(void* data, unsigned len);

// ID_FILE_HEADER ("1.00"/"2.00" text, group, unit count, then 0x88/0x8C byte units) plus the curves and
// paths its units point to. Returns 1 if everything was converted.
int XIdData_Swap(void* data, unsigned len);

// A TitleArc-style archive { u32 ofs[16] }: converts the offsets, then block i as IdTexData when bit i of texMask
// is set, as ID_FILE_HEADER when bit i of idMask is set. `len` may be 0 (unknown). Returns the number of
// blocks that failed (0 = all fine).
int XIdArc_Swap(void* arc, unsigned len, unsigned texMask, unsigned idMask);

#ifdef __cplusplus
}
#endif
#endif
