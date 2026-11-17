// The PS* wrappers (src/xbox/ps_wrappers.inc) forward every argument to their C_* twin and return its result.
// The twins are replaced by recorders (run_math_test.sh generates the rest as zero-returning weak stubs).
#include <dolphin/mtx.h>

#include <stdio.h>
#include <string.h>

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static const void *g_a0, *g_a1, *g_a2;
static u32 g_count;
static int g_called;

extern "C" {
void C_VECAdd(const Vec* a, const Vec* b, Vec* ab) { g_a0 = a; g_a1 = b; g_a2 = ab; g_called++; }
f32 C_VECDotProduct(const Vec* a, const Vec* b) { g_a0 = a; g_a1 = b; g_called++; return 42.5f; }
u32 C_MTXInverse(const Mtx src, Mtx inv) { g_a0 = src; g_a1 = inv; g_called++; return 7; }
void C_MTXMultVecArray(const Mtx m, const Vec* s, Vec* d, u32 n) { g_a0 = m; g_a1 = s; g_a2 = d; g_count = n; g_called++; }
void C_MTXRotRad(Mtx m, char axis, f32 rad) { g_a0 = m; g_count = (u32) axis; g_a1 = (const void*) (long) (int) (rad * 100); g_called++; }
}

int main()
{
    Vec a = {1, 2, 3}, b = {4, 5, 6}, c;
    Mtx m, n;
    PSVECAdd(&a, &b, &c);
    CHECK(g_called == 1 && g_a0 == &a && g_a1 == &b && g_a2 == &c);
    CHECK(PSVECDotProduct(&a, &b) == 42.5f && g_called == 2 && g_a0 == &a && g_a1 == &b);
    CHECK(PSMTXInverse(m, n) == 7 && g_a0 == m && g_a1 == n);
    PSMTXMultVecArray(m, &a, &b, 1234);
    CHECK(g_a0 == m && g_a1 == &a && g_a2 == &b && g_count == 1234);
    PSMTXRotRad(m, 'y', 0.5f);
    CHECK(g_a0 == m && g_count == 'y' && (long) g_a1 == 50);
    printf("%d checks, %d failures\n", g_n, g_bad);
    return g_bad != 0;
}
