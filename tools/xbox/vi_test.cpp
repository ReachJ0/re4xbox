// Host test of src/xbox/vi_xbox.cpp over a fake clock.
#include <stdio.h>
#include <dolphin/vi.h>
#include <xbox/xvi_backend.h>

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned long long g_now;
static int g_presents, g_lastBlack, g_lastW, g_lastH, g_sleeps;
static const void* g_lastFb;
extern "C" {
unsigned long long XViBackend_NowUs(void) { return g_now; }
void XViBackend_SleepUntilUs(unsigned long long t) { g_sleeps++; if (t > g_now) g_now = t; }
void XViBackend_Present(const void* fb, int black, int w, int h) { g_presents++; g_lastFb = fb; g_lastBlack = black; g_lastW = w; g_lastH = h; }
}

static u32 g_postCount, g_postLast, g_preCount;
static void Post(u32 n) { g_postCount++; g_postLast = n; }
static void Pre(u32) { g_preCount++; }

int main()
{
    g_now = 1000000;
    VIInit();
    CHECK(VIGetRetraceCount() == 0 && VIGetTvFormat() == VI_NTSC && VIGetDTVStatus() == 0);
    CHECK(VISetPostRetraceCallback(Post) == 0 && VISetPostRetraceCallback(Post) == Post);
    CHECK(VISetPreRetraceCallback(Pre) == 0);
    // time passes: 6 retraces of 16683us
    g_now += 6 * 16683 + 100;
    CHECK(VIGetRetraceCount() == 6 && g_postCount == 6 && g_postLast == 6 && g_preCount == 6 && g_presents == 6);
    // a stall is capped
    g_now += 100 * 16683;
    CHECK(VIGetRetraceCount() == 14 && g_postCount == 14);
    // configure, set a frame buffer and unblank: nothing changes until VIFlush + a retrace
    static GXRenderModeObj rm = {VI_TVMODE_PAL_INT, 640, 480, 574, 0, 0, 640, 574};
    char fbA[16], fbB[16];
    VIConfigure(&rm);
    VISetNextFrameBuffer(fbA);
    VISetBlack(0);
    CHECK(VIGetTvFormat() == VI_NTSC && VIGetCurrentFrameBuffer() == 0 && VIGetNextFrameBuffer() == fbA);
    g_now += 16683;
    VIGetRetraceCount();
    CHECK(g_lastBlack == 1 && g_lastFb == 0);       // still the old state at the retrace that just ran
    VIFlush();
    VIWaitForRetrace();                             // sleeps to the next retrace
    CHECK(g_sleeps >= 1 && g_lastFb == fbA && g_lastBlack == 0 && g_lastW == 640 && g_lastH == 574);
    CHECK(VIGetTvFormat() == VI_PAL && VIGetCurrentFrameBuffer() == fbA);
    // PAL: 20000us retraces
    unsigned before = VIGetRetraceCount();
    g_now += 5 * 20000;
    CHECK(VIGetRetraceCount() == before + 5);
    // double buffering: next buffer applies at the retrace after the flush
    VISetNextFrameBuffer(fbB);
    CHECK(VIGetCurrentFrameBuffer() == fbA);
    VIFlush();
    VIWaitForRetrace();
    CHECK(VIGetCurrentFrameBuffer() == fbB && g_lastFb == fbB);
    // field alternates each retrace
    u32 f0 = VIGetNextField();
    VIWaitForRetrace();
    CHECK(VIGetNextField() == (f0 ^ 1));
    // callback return value, removal
    CHECK(VISetPostRetraceCallback(0) == Post);
    unsigned pc = g_postCount;
    VIWaitForRetrace();
    CHECK(g_postCount == pc);
    printf("vi: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
