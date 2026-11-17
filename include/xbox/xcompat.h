// Dialect differences between the original compiler (GCC 2.95 / MWCC) and clang C++ (nxdk).
// Only included under RE4_XBOX; the GameCube build never sees it.
#ifndef XBOX_XCOMPAT_H
#define XBOX_XCOMPAT_H
#ifdef RE4_XBOX
#ifdef __cplusplus
#include <string.h>
#include <new>
// C++ <string.h> returns const char* for const input; the 2.95 libc returned char*.
#define strstr(a, b) ((char*) strstr((a), (b)))
#define strchr(a, b) ((char*) strchr((a), (b)))
#define strrchr(a, b) ((char*) strrchr((a), (b)))
#define PLACEMENT_NEW_DEFINED 1
// GCC/PowerPC `asm(...)`: scheduling barriers ("" : "+f"(x)), section directives, label renames and
// hand-written PPC blocks. None of it means anything on x86; the PPC blocks that carry logic
// (memset_2, yz2asm) are replaced by C. `asm volatile(...)` is NOT touched: those are reviewed one by one.
#define asm(...)
#define __asm__(...)
#endif
#endif
#endif
