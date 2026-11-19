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

#ifndef NULL
#define NULL 0
#endif

// Game memory map (RE4_XBOX: configurable base; GameCube: the original literals).
#include "xbox/xmem.h"
#include "xbox/xgxfifo.h"
#include "xbox/xppc.h"
#include "xbox/xcompat.h"

#endif
