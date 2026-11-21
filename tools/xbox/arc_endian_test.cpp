// Host test for src/xbox/xarc_endian.cpp: an archive offset table built big-endian, converted, read as CORE_ARC does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "xbox/xarc_endian.h"
static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void P32(unsigned char* b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void P16(unsigned char* b, unsigned v) { b[0] = v >> 8; b[1] = v; }
static void TestCardArc()
{
    // [0..3] header, [4..9] ofs: TPL at 0x40, message table at 0x100; the TPL has one descriptor with a 0x24 header
    unsigned char a[0x180] = {0};
    P32(a + 16, 0x40); P32(a + 20, 0x100);
    for (int i = 2; i < 6; i++) P32(a + 16 + i * 4, 0x160 + i * 4);
    unsigned char* t = a + 0x40;
    P32(t, 0x20AF30); P32(t + 4, 1); P32(t + 8, 0xC); P32(t + 0xC, 0x20); P32(t + 0x10, 0);
    P16(t + 0x20, 64); P16(t + 0x22, 32); P32(t + 0x24, 5); P32(t + 0x28, 0x60);
    P32(a + 0x100, 2); P32(a + 0x104, 8); P32(a + 0x108, 0x43EAB241); P32(a + 0x10C, 1); P32(a + 0x110, 0xC); P16(a + 0x114, 0x0041);   // message table
    CHECK(XArc_SwapCardArc(a, sizeof a) == 1);
    CHECK(((uint32_t*) a)[4] == 0x40 && ((uint32_t*) a)[5] == 0x100);
    CHECK(*(uint32_t*) (t + 4) == 1 && *(uint32_t*) (t + 0xC) == 0x20 && *(uint16_t*) (t + 0x20) == 64 && *(uint32_t*) (t + 0x24) == 5);
    CHECK(*(uint32_t*) (a + 0x100) == 2 && *(uint32_t*) (a + 0x104) == 8 && *(uint32_t*) (a + 0x108) == 0x43EAB241 && *(uint32_t*) (a + 0x110) == 0xC && *(uint16_t*) (a + 0x114) == 0x41);   // messages converted
    unsigned char bad[0x40]; memset(bad, 0xEE, sizeof bad);
    CHECK(XArc_SwapCardArc(bad, sizeof bad) == 0);
    unsigned char* g = (unsigned char*) malloc(0x50);
    memcpy(g, a, 0x50); memset(g, 0, 16); P32(g + 16, 0x40); P32(g + 20, 0x100);   // TPL cut by the buffer end
    CHECK(XArc_SwapCardArc(g, 0x50) == 0);   // nothing read past the buffer (ASan)
    free(g);
}

int main()
{
    TestCardArc();
    unsigned char a[0x200] = {0};
    // 4 header words, then offsets: first sub-file at 0x40 => the table is 16 words
    P32(a, 0x20); P32(a + 4, 1); P32(a + 8, 2); P32(a + 12, 3);
    for (int i = 4; i < 16; i++) P32(a + i * 4, 0x40 + (i - 4) * 0x10);
    P32(a + 0x40, 0xDEADBEEF);   // sub-file data: must stay big-endian
    unsigned char orig[0x200]; memcpy(orig, a, sizeof a);
    CHECK(XArc_SwapTable(a, sizeof a, 4) == 16);
    CHECK(((uint32_t*) a)[0] == 0x20 && ((uint32_t*) a)[3] == 3 && ((uint32_t*) a)[4] == 0x40 && ((uint32_t*) a)[15] == 0x40 + 11 * 0x10);
    CHECK(a[0x40] == 0xDE && a[0x43] == 0xEF);
    // hostile: every length (exact heap buffers) and garbage
    for (unsigned len = 0; len < 0x200; len += 3) {
        unsigned char* g = (unsigned char*) malloc(len ? len : 1);
        memcpy(g, orig, len);
        unsigned n = XArc_SwapTable(g, len, 4);
        CHECK(n == 0 || n * 4 <= len);
        free(g);
    }
    unsigned char bad[64]; memset(bad, 0xFF, sizeof bad);
    CHECK(XArc_SwapTable(bad, sizeof bad, 4) == 0 && ((uint32_t*) bad)[4] == 0xFFFFFFFFu);
    unsigned char z[64] = {0};
    CHECK(XArc_SwapTable(z, sizeof z, 4) == 0);   // first sub-file at 0: not a table
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
