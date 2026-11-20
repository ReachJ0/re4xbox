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

static int TestOthers()
{
    // bio4midi.hed: array of u32
    unsigned char a[16];
    for (int i = 0; i < 4; i++) P32(a + i * 4, 0x60 + i);
    CHECK(XSnd_SwapU32Array(a, 16) == 1 && ((uint32_t*) a)[0] == 0x60 && ((uint32_t*) a)[3] == 0x63);

    // doorse.hed: { file_ofs 0x10, num_ofs 0x08 }, u16 count 3 at 8, u32 files at 0x10
    unsigned char d[0x20] = {0};
    P32(d, 0x10); P32(d + 4, 0x08); P16(d + 8, 3);
    for (int i = 0; i < 3; i++) P32(d + 0x10 + i * 4, 0x100 + i);
    CHECK(XSnd_SwapDoorTbl(d, sizeof d) == 1);
    CHECK(*(uint32_t*) d == 0x10 && *(uint16_t*) (d + 8) == 3 && ((uint32_t*) (d + 0x10))[2] == 0x102);

    // bgmtbl.dat: list at 0x08 (ids 5, 9, 11, 0xFFFF), room offsets at 0x10 (3 rooms; rooms 1 and 2 share a block)
    unsigned char b[0x200] = {0};
    P32(b, 0x10); P32(b + 4, 0x08);
    P16(b + 8, 5); P16(b + 10, 9); P16(b + 12, 11); P16(b + 14, 0xFFFF);
    unsigned blk0 = 0x40, blk1 = 0x100;
    P32(b + 0x10, blk0 - 0x10); P32(b + 0x14, blk1 - 0x10); P32(b + 0x18, blk1 - 0x10);
    P32(b + blk0, 2);                        // 2 entries
    for (int k = 0; k < 26; k++) P32(b + blk0 + 4 + k * 4, 0x1000 + k);
    P32(b + blk1, 1);
    for (int k = 0; k < 13; k++) P32(b + blk1 + 4 + k * 4, 0x2000 + k);
    CHECK(XSnd_SwapBgmTbl(b, sizeof b) == 1);
    CHECK(*(uint32_t*) b == 0x10 && *(uint32_t*) (b + 4) == 8 && *(uint16_t*) (b + 14) == 0xFFFF && *(uint16_t*) (b + 10) == 9);
    CHECK(*(uint32_t*) (b + 0x10) == blk0 - 0x10 && *(uint32_t*) (b + 0x18) == blk1 - 0x10);
    CHECK(*(uint32_t*) (b + blk0) == 2 && *(uint32_t*) (b + blk0 + 4 + 25 * 4) == 0x1000 + 25);
    CHECK(*(uint32_t*) (b + blk1) == 1 && *(uint32_t*) (b + blk1 + 4 + 12 * 4) == 0x2000 + 12);   // shared block: once
    // hostile: truncated at every size, exact heap buffers
    unsigned char orig[0x200];
    memcpy(orig, b, sizeof b);   // (already converted: any content is fine for robustness)
    for (unsigned len = 0; len < 0x200; len += 5) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        memcpy(g, orig, len);
        XSnd_SwapBgmTbl(g, len);
        XSnd_SwapDoorTbl(g, len);
        XSnd_SwapU32Array(g, len);
        free(g);
    }
    return 0;
}

static void TestIss()
{
    // outer block: [u32 hdr offset 0x20] ... header at 0x20 {sit_num 2, wt 0x40, sit 0x30, seq 0x70}, SIT[2] at hdr+0x30,
    // sequence table at hdr+0x70 {count 2, ofs 0x10 0x20}
    unsigned char b[0x200] = {0};
    P32(b, 0x20);
    unsigned h = 0x20;
    P32(b + h, 2); P32(b + h + 4, 0x40); P32(b + h + 8, 0x30); P32(b + h + 12, 0x70);
    for (int i = 0; i < 2; i++) {
        unsigned char* e = b + h + 0x30 + i * 0x18;
        P16(e, 0x0100 + i); e[2] = 1; e[6] = 100; P16(e + 0x0A, 5); P16(e + 0x0C, 9); P16(e + 0x16, 0x8004);
    }
    P32(b + h + 0x70, 2); P32(b + h + 0x74, 0x10); P32(b + h + 0x78, 0x20);
    CHECK(XSnd_SwapIssBlk(b, sizeof b, 1) == 1);
    CHECK(*(uint32_t*) b == 0x20 && *(uint32_t*) (b + h) == 2 && *(uint32_t*) (b + h + 8) == 0x30);
    unsigned char* e1 = b + h + 0x30 + 0x18;
    CHECK(*(uint16_t*) e1 == 0x0101 && e1[2] == 1 && e1[6] == 100 && *(uint16_t*) (e1 + 0x0A) == 5 && *(uint16_t*) (e1 + 0x0C) == 9 && *(uint16_t*) (e1 + 0x16) == 0x8004);
    CHECK(*(uint32_t*) (b + h + 0x70) == 2 && *(uint32_t*) (b + h + 0x78) == 0x20);
    // no outer offset (BGM blocks): header at 0
    unsigned char c[0x100] = {0};
    P32(c, 1); P32(c + 4, 0x40); P32(c + 8, 0x30); P32(c + 12, 0x60);
    P16(c + 0x30, 7); P32(c + 0x60, 0);
    CHECK(XSnd_SwapIssBlk(c, sizeof c, 0) == 1 && *(uint16_t*) (c + 0x30) == 7 && *(uint32_t*) (c + 8) == 0x30);
    // hostile: every length, exact heap buffers
    unsigned char big[0x200];
    memcpy(big, b, sizeof big);
    for (unsigned len = 0; len < 0x200; len += 3) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        memcpy(g, big, len);
        XSnd_SwapIssBlk(g, len, 1);
        XSnd_SwapIssBlk(g, len, 0);
        free(g);
    }
    unsigned char* bad = (unsigned char*) malloc(40);
    memset(bad, 0xFF, 40);
    CHECK(XSnd_SwapIssBlk(bad, 40, 1) == 0 && XSnd_SwapIssBlk(bad, 40, 0) == 0);
    free(bad);
}

int main()
{
    TestIss();
    TestOthers();
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
