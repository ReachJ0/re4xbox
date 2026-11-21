#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef signed short s16;
typedef signed long s32;
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;
typedef int BOOL;

// Xbox build: the decompiled source sometimes falls off the end of a function that returns a value (the PowerPC
// code left the result in r3). Clang turns that into an illegal instruction, so those places write `XRET value;`.
// In the GameCube build XRET is empty and the statement has no effect.
// XRETV(value) is the same for a function that falls off the end with no expression to return: nothing at all
// in the GameCube build.
#ifdef RE4_XBOX
#define XRET return
#define XRETV(v) return v;
#else
#define XRET
#define XRETV(v)
#endif

// The game waits for vsync with an empty loop (`while (vsync_cnt < n) {}`); the counter is raised by the VI
// post-retrace callback, an interrupt on the GameCube. The Xbox VI runs its retraces on a virtual clock whenever
// the game calls into VI, so an empty loop would never see the counter move: here the loop waits for the next
// retrace, and the callbacks run on the game's own thread. Nothing changes in the GameCube build.
#ifdef RE4_XBOX
#ifdef __cplusplus
extern "C" void VIWaitForRetrace(void);
#else
void VIWaitForRetrace(void);
#endif
#define SPIN_VSYNC(cond) while (cond) { VIWaitForRetrace(); }
#include "xbox/xtrace.h"   // development traces (XFRM*, XCARD_TRACE, RE4_TRACE)
// sub screen data (SndMem.sub_adr): big-endian offset table + icon TPL + messages, converted after it is read
#ifdef __cplusplus
extern "C" {
#endif
void XSub_Convert(void* data, unsigned len);
#ifdef __cplusplus
}
#endif
#define XSUBDATA(adr, len) XSub_Convert((void*) (adr), (unsigned) (len));
#else
#define SPIN_VSYNC(cond) while (cond) {}
#define XFRM_DECL
#define XSUBDATA(adr, len)
#define XMARK(n)
#define XCARD_TRACE(a, b, c, d)
#define XFRM(txt)
#define XFRM_END
#define XFRMV(txt, a, b)
#endif

#ifndef NULL
#define NULL 0
#endif

// Game memory map (RE4_XBOX: configurable base; GameCube: the original literals).
#include "xbox/xmem.h"
#include "xbox/xgxfifo.h"
#include "xbox/xppc.h"
#include "xbox/xcompat.h"

#endif
