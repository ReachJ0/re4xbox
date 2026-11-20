// Host test for src/xbox/xsnd_endian.cpp: builds a bio4str.hed-like file in big-endian, converts it, and reads it
// back the way the game does (as native u32 / s16 / u16). Also feeds it truncated and hostile files.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "xbox/xsnd_endian.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void P32(unsigned char* b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void P16(unsigned char* b, unsigned v) { b[0] = v >> 8; b[1] = v; }

// one block at `base`: 3 RIT entries (str_no 0,1,1), 2 stream headers
static void MakeBlock(unsigned char* f, unsigned base, unsigned seed)
{
    unsigned ritOfs = 0x20, sbhOfs = 0x20 + 3 * 0x10;
    P32(f + base, 3);
    P32(f + base + 4, sbhOfs);
    P32(f + base + 8, ritOfs);
    static const int strNo[3] = {0, 1, 1};
    for (int i = 0; i < 3; i++) {
        unsigned char* r = f + base + ritOfs + i * 0x10;
        P16(r, strNo[i]);
        r[2] = 5 + i; r[3] = 2; r[4] = 100; r[0xA] = 7;   // single bytes: must stay put
        P16(r + 0x0C, 0x0101 + i);
    }
    unsigned char* t = f + base + sbhOfs;
    P32(t, 8); P32(t + 4, 8 + 0x7C);
    for (int s = 0; s < 2; s++) {
        unsigned char* h = t + 8 + s * 0x7C;
        for (int k = 0; k < 8; k++) P32(h + k * 4, seed + s * 100 + k + 0x10000);
        for (int k = 0; k < 46; k++) P16(h + 0x20 + k * 2, 0x8000 + seed + s * 10 + k);
    }
}

int main()
{
    enum { N = 0x400 };
    unsigned char* f = (unsigned char*) calloc(1, N);
    P32(f, 0x20);
    P32(f + 4, 0x200);
    MakeBlock(f, 0x20, 1);
    MakeBlock(f, 0x200, 2);
    unsigned char* orig = (unsigned char*) malloc(N);
    memcpy(orig, f, N);

    CHECK(XSnd_SwapStrHed(f, N) == 1);
    CHECK(*(uint32_t*) f == 0x20 && *(uint32_t*) (f + 4) == 0x200);   // native reads (host is little-endian)
    for (int b = 0; b < 2; b++) {
        unsigned base = b ? 0x200 : 0x20;
        CHECK(*(uint32_t*) (f + base) == 3);
        CHECK(*(uint32_t*) (f + base + 4) == 0x50 && *(uint32_t*) (f + base + 8) == 0x20);
        int16_t* s0 = (int16_t*) (f + base + 0x20 + 0x10);
        CHECK(*s0 == 1);
        CHECK(*(uint16_t*) (f + base + 0x20 + 0x10 + 0x0C) == 0x0102);
        CHECK(f[base + 0x20 + 2] == 5 && f[base + 0x20 + 4] == 100 && f[base + 0x20 + 0xA] == 7);
        unsigned char* t = f + base + 0x50;
        CHECK(*(uint32_t*) t == 8 && *(uint32_t*) (t + 4) == 8 + 0x7C);
        unsigned char* h1 = t + 8 + 0x7C;
        CHECK(*(uint32_t*) (h1 + 12) == (unsigned) (b + 1 + 100 + 3 + 0x10000));
        CHECK(*(uint16_t*) (h1 + 0x20 + 45 * 2) == 0x8000 + (b + 1) + 10 + 45);
        CHECK(*(uint16_t*) (h1 + 0x7A) == 0x8000 + (b + 1) + 10 + 45);
    }

    // hostile files: must not read or write outside the buffer (run under ASan) and must report failure
    for (unsigned len = 0; len < N; len += 7) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);   // exact size so ASan sees an overrun
        memcpy(g, orig, len);
        int r = XSnd_SwapStrHed(g, len);
        if (len < N) CHECK(r == 0 || len >= 0x200 + 0x50 + 8 + 2 * 0x7C);
        free(g);
    }
    unsigned char* bad = (unsigned char*) malloc(64);
    memset(bad, 0xFF, 64);
    CHECK(XSnd_SwapStrHed(bad, 64) == 0);
    free(bad);
    free(orig);
    free(f);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
