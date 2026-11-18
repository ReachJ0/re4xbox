// Write-gather FIFO of the GX command processor. On the GameCube the game and the SDK store into a fixed address
// (`GXWGFifo.f32 = x`); on the Xbox every store is a call into the software command processor (src/xbox/gp_*).
// GXFIFO_WRITE is the `.` form (SDK), GXFIFOP_WRITE the `->` form (include/gx.h, game units); on the GameCube both
// expand to the original statement.
#ifndef XBOX_XGXFIFO_H
#define XBOX_XGXFIFO_H
#ifdef RE4_XBOX
#ifdef __cplusplus
extern "C" {
#endif
void XGx_W_u8(unsigned char v);
void XGx_W_s8(signed char v);
void XGx_W_u16(unsigned short v);
void XGx_W_s16(short v);
void XGx_W_u32(unsigned int v);
void XGx_W_s32(int v);
void XGx_W_f32(float v);
#ifdef __cplusplus
}
#endif
#define GXFIFO_WRITE(T, x) XGx_W_##T(x)
#define GXFIFOP_WRITE(T, x) XGx_W_##T(x)
#else
#define GXFIFO_WRITE(T, x) GXWGFifo.T = x
#define GXFIFOP_WRITE(T, x) GXWGFifo->T = x
#endif
#endif
