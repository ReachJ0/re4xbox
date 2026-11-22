// Host test for src/xbox/xidtex_endian.cpp: id texture pack + widget table + archive built big-endian, converted, read back.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xbox/xidtex_endian.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void P32(unsigned char* b, unsigned v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static void P16(unsigned char* b, unsigned v) { b[0] = v >> 8; b[1] = v; }
static void PF(unsigned char* b, float f) { uint32_t u; memcpy(&u, &f, 4); P32(b, u); }
static uint32_t R32(const unsigned char* b) { uint32_t v; memcpy(&v, b, 4); return v; }
static uint16_t R16(const unsigned char* b) { uint16_t v; memcpy(&v, b, 2); return v; }
static float RF(const unsigned char* b) { float v; memcpy(&v, b, 4); return v; }

// IdTexData: id table @0x40 (2 entries), TPL table @0x60, Anm table @0x70, TPLs @0x80/@0xE0, anms @0x140/@0x160
static unsigned MakeTex(unsigned char* d)
{
    P32(d, 0xB); P32(d + 4, 0x40); P32(d + 0x18, 0x60); P32(d + 0x1C, 0x70);
    P32(d + 0x40, 2);
    P16(d + 0x44, 7); P16(d + 0x46, 1); P32(d + 0x48, 0x55);
    P16(d + 0x4C, 0x80); P16(d + 0x4E, 2); P32(d + 0x50, 0x66);
    P32(d + 0x60, 2); P32(d + 0x64, 0x20); P32(d + 0x68, 0x80);      // TPLs at 0x80 and 0xE0
    P32(d + 0x70, 2); P32(d + 0x74, 0xD0); P32(d + 0x78, 0xF0);      // anms at 0x140 and 0x160
    for (int i = 0; i < 2; i++) {
        unsigned char* t = d + 0x80 + i * 0x60;
        P32(t, 0x20AF30); P32(t + 4, 1); P32(t + 8, 0xC);
        P32(t + 0xC, 0x18); P32(t + 0x10, 0);
        unsigned char* h = t + 0x18;
        P16(h, 32 + i); P16(h + 2, 16); P32(h + 4, 5); P32(h + 8, 0x200);
        P32(h + 12, 1); P32(h + 16, 1); P32(h + 20, 1); P32(h + 24, 1); PF(h + 28, 0.0f);
        unsigned char* a = d + 0x140 + i * 0x20;
        P16(a, 64 + i); P16(a + 2, 32); P16(a + 4, 5); P16(a + 6, 6); P16(a + 8, 3 + i);
    }
    return 0x180;
}

// ID_FILE_HEADER v2, 1 unit with a curve (2 points) and a path (2 points)
static unsigned MakeId(unsigned char* d)
{
    memcpy(d, "2.00", 4); d[4] = 1; d[5] = 1;
    unsigned char* u = d + 8;
    u[3] = 0x0D; u[4] = 5;
    PF(u + 0x14, 1.5f); PF(u + 0x18, 2.5f); PF(u + 0x1C, 3.5f);
    PF(u + 0x20, -1.0f); PF(u + 0x50, 64.0f); PF(u + 0x54, 32.0f); PF(u + 0x60, 0.25f);
    P32(u + 0x74, 0x120);                    // path0
    P32(u + 0x78, 0x140);                    // path1 (work area: must stay as it is)
    P32(u + 0x7C, 0x180);                    // curve0
    unsigned char* p = d + 0x120; p[0] = 3; p[7] = 2; PF(p + 8, 4.0f); PF(p + 0x1C, 9.0f);
    memset(d + 0x140, 0x5A, 8);
    unsigned char* c = d + 0x180; P32(c, 2); PF(c + 4, 0.0f); PF(c + 8, 10.0f); PF(c + 0x1C, 1.0f); PF(c + 0x20, 20.0f);
    return 0x1C0;
}

int main()
{
    static unsigned char a[0x800];
    // archive: ofs[4] = tex @0x80, ofs[6] = id @0x300
    P32(a + 16, 0x80); P32(a + 24, 0x300);
    unsigned tn = MakeTex(a + 0x80); (void) tn;
    MakeId(a + 0x300);
    CHECK(XIdArc_Swap(a, 0x800, 1u << 4, 1u << 6) == 0);
    CHECK(R32(a + 16) == 0x80 && R32(a + 24) == 0x300);
    unsigned char* t = a + 0x80;
    CHECK(R32(t) == 0xB && R32(t + 4) == 0x40 && R32(t + 0x18) == 0x60 && R32(t + 0x1C) == 0x70);
    CHECK(R32(t + 0x40) == 2 && R16(t + 0x44) == 7 && R16(t + 0x46) == 1 && R32(t + 0x48) == 0x55 && R16(t + 0x4C) == 0x80);
    CHECK(R32(t + 0x68) == 0x80 && R32(t + 0x78) == 0xF0);
    CHECK(R32(t + 0x80) == 0x20AF30 && R32(t + 0x80 + 0xC) == 0x18 && R16(t + 0x80 + 0x18) == 32 && R16(t + 0xE0 + 0x18) == 33);
    CHECK(R16(t + 0x140) == 64 && R16(t + 0x142) == 32 && R16(t + 0x144) == 5 && R16(t + 0x148) == 3 && R16(t + 0x168) == 4);
    unsigned char* d = a + 0x300;
    CHECK(memcmp(d, "2.00", 4) == 0 && d[5] == 1);
    CHECK(RF(d + 8 + 0x14) == 1.5f && RF(d + 8 + 0x1C) == 3.5f && RF(d + 8 + 0x20) == -1.0f);
    CHECK(RF(d + 8 + 0x50) == 64.0f && RF(d + 8 + 0x54) == 32.0f && RF(d + 8 + 0x60) == 0.25f);
    CHECK(R32(d + 8 + 0x74) == 0x120 && R32(d + 8 + 0x78) == 0x140 && R32(d + 8 + 0x7C) == 0x180);
    CHECK(d[0x120] == 3 && d[0x127] == 2 && RF(d + 0x128) == 4.0f && RF(d + 0x13C) == 9.0f);
    CHECK(d[0x140] == 0x5A && d[0x147] == 0x5A);
    CHECK(R32(d + 0x180) == 2 && RF(d + 0x188) == 10.0f && RF(d + 0x1A0) == 20.0f);
    // garbage is refused
    unsigned char g[0x100]; memset(g, 0xFF, sizeof g);
    CHECK(XIdTex_Swap(g, sizeof g) == 0 && XIdData_Swap(g, sizeof g) == 0);
    unsigned char z[0x100] = {0};
    CHECK(XIdArc_Swap(z, 0x100, 1u << 4, 0) == 1);
    printf("%d checks, %d fail\n", g_checks, g_fail);
    return g_fail != 0;
}
