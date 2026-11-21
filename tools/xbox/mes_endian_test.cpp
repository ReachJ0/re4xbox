// Host test for src/xbox/xmes_endian.cpp: a message file built big-endian, converted, read as MessageData::getAddr does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "xbox/xmes_endian.h"
static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void P32(unsigned char* b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void P16(unsigned char* b, unsigned v) { b[0] = v >> 8; b[1] = v; }

// table: [?][lang0][lang1][lang2 = same block as lang1]; lang0: 2 messages, lang1: 1 message
static unsigned Make(unsigned char* d)
{
    P32(d, 3); P32(d + 4, 0x10); P32(d + 8, 0x30); P32(d + 12, 0x30);
    unsigned char* b0 = d + 0x10;                      // 0x10 .. 0x30
    P32(b0, 2); P32(b0 + 4, 0xC); P32(b0 + 8, 0x10);
    P16(b0 + 0xC, 0x0041); P16(b0 + 0xE, 0x0000); P16(b0 + 0x10, 0x1234); P16(b0 + 0x12, 0xFFFF);
    unsigned char* b1 = d + 0x30;                      // 0x30 .. 0x40
    P32(b1, 1); P32(b1 + 4, 0x8); P16(b1 + 8, 0x5678); P16(b1 + 10, 0x0000);
    return 0x40;
}

int main()
{
    unsigned char d[0x40] = {0}, orig[0x40];
    unsigned n = Make(d);
    memcpy(orig, d, n);
    CHECK(XMes_Swap(d, n) == 2);
    uint32_t* t = (uint32_t*) d;
    CHECK(t[0] == 3 && t[1] == 0x10 && t[2] == 0x30 && t[3] == 0x30);
    uint32_t* b0 = (uint32_t*) (d + 0x10);
    CHECK(b0[0] == 2 && b0[1] == 0xC && b0[2] == 0x10);
    uint16_t* s0 = (uint16_t*) (d + 0x10 + 0xC);
    CHECK(s0[0] == 0x41 && s0[1] == 0 && s0[2] == 0x1234 && s0[3] == 0xFFFF);
    CHECK(*(uint32_t*) (d + 0x30) == 1 && *(uint32_t*) (d + 0x34) == 8 && *(uint16_t*) (d + 0x38) == 0x5678);

    // hostile: every length on exact heap buffers, and garbage
    for (unsigned len = 0; len <= n; len++) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        memcpy(g, orig, len);
        int r = XMes_Swap(g, len);
        CHECK(r == 0 || r == 2);
        free(g);
    }
    unsigned char bad[64]; memset(bad, 0xFF, sizeof bad);
    CHECK(XMes_Swap(bad, sizeof bad) == 0 && bad[0] == 0xFF);
    unsigned char big[64]; memcpy(big, orig, n); P32(big + 0x10, 0x7FFFFFFF);   // count that does not fit
    CHECK(XMes_Swap(big, n) == 0 && big[0x10] == 0x7F);
    unsigned char lowOff[0x40]; memcpy(lowOff, orig, n); P32(lowOff + 8, 0x4);  // offset inside the table
    CHECK(XMes_Swap(lowOff, n) == 0);
    for (int i = 0; i < 20000; i++) {   // random garbage never reads past the buffer (ASan)
        unsigned len = (unsigned) (rand() % 80);
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        for (unsigned k = 0; k < len; k++) g[k] = (unsigned char) rand();
        if (len >= 8 && (i & 1)) { P32(g + 4, 8 + 4 * (rand() % 8)); }
        XMes_Swap(g, len);
        free(g);
    }
    printf("mes: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
