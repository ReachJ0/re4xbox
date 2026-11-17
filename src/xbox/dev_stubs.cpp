// Development-only services of the original (SN Systems PC file server, Dolphin debugger, PPC sync) and the
// GCC 2.95 allocation entry points. None of them has a meaning on the retail Xbox build.
#include <stddef.h>
#include <string.h>
#include <new>

extern "C" {

// ProDG PC file server: the game only uses it when SYS_SN_PC_READ is set (never in retail); every call fails.
int PCinit() { return -1; }
int PCcreat(const char*, int) { return -1; }
int PCopen(const char*, int, int) { return -1; }
int PCclose(int) { return -1; }
int PCread(int, void*, int) { return -1; }
int PCwrite(int, const void*, int) { return -1; }
int PClseek(int, int, int) { return -1; }

// No debugger is ever attached.
int DBIsDebuggerPresent(void) { return 0; }

// `sync` orders PowerPC bus accesses; x86 is already ordered for the accesses the game makes.
void PPCSync(void) { __asm__ __volatile__("" ::: "memory"); }

// IPL ROM font (error screens only: dvd.cpp RomFontPrint). The Dolphin IPL fonts are Nintendo's and are not
// available on the Xbox; until a replacement font ships, the encoding is ANSI, OSInitFont reports failure and
// OSGetFontTexture consumes the whole string and returns no image (the draw code must skip a null image).
unsigned short OSGetFontEncode(void) { return 0; }
int OSInitFont(void*) { return 0; }
char* OSGetFontTexture(const char* s, void** image, int* x, int* y, int* width) {
    *image = 0; *x = 0; *y = 0; *width = 0;
    while (*s) s++;
    return (char*) s;
}

// GCC 2.95 operator new/delete (the original aliased them with asm labels).
void* __builtin_new(size_t n) { return ::operator new(n); }
void* __builtin_vec_new(size_t n) { return ::operator new[](n); }
void __builtin_delete(void* p) { ::operator delete(p); }
void __builtin_vec_delete(void* p) { ::operator delete[](p); }

// route_ck.cpp's `memset_v(...)` aliases libc memset.
void* memset_v(void* d, int c, size_t n) { return memset(d, c, n); }

}
