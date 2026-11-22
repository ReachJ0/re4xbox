// Host test of the big-endian GetDataExt archive header access (src/xbox/xdataext.cpp).
#include "xbox/xdataext.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks, g_traces;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void Trace(const char* t) { g_traces++; if (strncmp(t, "DataExt ", 8) != 0) g_fail++; }
static void P32(unsigned char* p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int main()
{
    unsigned char a[128];
    memset(a, 0, sizeof(a));
    P32(a, 2);                       // two blocks
    P32(a + 0x10, 0x28); P32(a + 0x14, 0x30);
    memcpy(a + 0x18, "FSE\0SMD\0", 8);
    CHECK(XDataExt_Num(a) == 2);
    CHECK(XDataExt_Block(a, 0, "FSE", Trace) == 0 && g_traces == 1);   // no converter yet: NULL and a trace
    CHECK(XDataExt_Block(a, 5, "FSE", Trace) == 0);                    // out of range
    memset(a, 0xFF, 4);
    CHECK(XDataExt_Num(a) == 0);                                        // implausible count
    printf("dataext: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
