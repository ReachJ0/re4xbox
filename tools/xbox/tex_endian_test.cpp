// Host test for src/xbox/xtex_endian.cpp: a TPL / .fnt built big-endian, converted, read back as the game does.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "xbox/xtex_endian.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void P32(unsigned char* b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void P16(unsigned char* b, unsigned v) { b[0] = v >> 8; b[1] = v; }

// TPL at 0: 3 descriptors; 0 and 1 have their own headers (1 is CI with a CLUT), 2 shares descriptor 0's headers
static unsigned MakeTpl(unsigned char* t)
{
    P32(t, 0x20AF30); P32(t + 4, 3); P32(t + 8, 0xC);
    unsigned d = 0xC, th0 = 0x30, th1 = 0x60, cl1 = 0x90;
    P32(t + d, th0);      P32(t + d + 4, 0);
    P32(t + d + 8, th1);  P32(t + d + 12, cl1);
    P32(t + d + 16, th0); P32(t + d + 20, 0);
    unsigned h[2] = {th0, th1};
    for (int i = 0; i < 2; i++) {
        unsigned char* x = t + h[i];
        P16(x, 64 + i); P16(x + 2, 128 + i); P32(x + 4, i ? 8 : 5); P32(x + 8, 0x200 + i);
        P32(x + 12, 1); P32(x + 16, 2); P32(x + 20, 1); P32(x + 24, 1);
        float f = 0.5f; uint32_t fb; memcpy(&fb, &f, 4); P32(x + 28, fb);
        x[0x20] = 1; x[0x21] = 2; x[0x22] = 3; x[0x23] = 0;
    }
    unsigned char* c = t + cl1;
    P16(c, 256); c[2] = 0; c[3] = 0; P32(c + 4, 2); P32(c + 8, 0x300);
    return 0xA0;
}

int main()
{
    unsigned char t[0x100] = {0};
    unsigned n = MakeTpl(t);
    unsigned char orig[0x100]; memcpy(orig, t, sizeof t);
    CHECK(XTex_SwapTpl(t, n) == 1);
    CHECK(*(uint32_t*) t == 0x20AF30 && *(uint32_t*) (t + 4) == 3 && *(uint32_t*) (t + 8) == 0xC);
    CHECK(*(uint32_t*) (t + 0xC) == 0x30 && *(uint32_t*) (t + 0x14) == 0x60 && *(uint32_t*) (t + 0x18) == 0x90);
    CHECK(*(uint32_t*) (t + 0x1C) == 0x30);                       // shared header, descriptor 2
    CHECK(*(uint16_t*) (t + 0x30) == 64 && *(uint16_t*) (t + 0x32) == 128 && *(uint32_t*) (t + 0x34) == 5);
    CHECK(*(uint32_t*) (t + 0x38) == 0x200 && *(uint32_t*) (t + 0x3C) == 1 && *(uint32_t*) (t + 0x4C - 4) == 1);
    CHECK(*(float*) (t + 0x30 + 28) == 0.5f);
    CHECK(t[0x50] == 1 && t[0x51] == 2 && t[0x52] == 3);          // bytes untouched
    CHECK(*(uint16_t*) (t + 0x60) == 65 && *(uint32_t*) (t + 0x64) == 8 && *(uint32_t*) (t + 0x68) == 0x201);
    CHECK(*(uint16_t*) (t + 0x90) == 256 && *(uint32_t*) (t + 0x94) == 2 && *(uint32_t*) (t + 0x98) == 0x300);

    // .fnt: [tpl offset][width offset] ... TPL at 0x20
    unsigned char f[0x200] = {0};
    P32(f, 0x20); P32(f + 4, 0x1C0);
    memcpy(f + 0x20, orig, n);
    CHECK(XTex_SwapFnt(f, 0x20 + n) == 1);
    CHECK(*(uint32_t*) f == 0x20 && *(uint32_t*) (f + 4) == 0x1C0 && *(uint32_t*) (f + 0x20 + 4) == 3);

    // hostile input: exact-size heap buffers so ASan sees any overrun
    for (unsigned len = 0; len <= n; len += 3) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        memcpy(g, orig, len);
        int r = XTex_SwapTpl(g, len);
        CHECK(r == 0 || len >= n - 0x10);
        free(g);
    }
    unsigned char* bad = (unsigned char*) malloc(32);
    memset(bad, 0xFF, 32);
    CHECK(XTex_SwapTpl(bad, 32) == 0 && XTex_SwapFnt(bad, 32) == 0);
    free(bad);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
