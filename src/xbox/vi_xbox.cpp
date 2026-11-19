// Video interface: the Dolphin VI calls the game makes, over a virtual retrace clock (see xvi_backend.h).
//
// Semantics kept from the SDK (VI.c):
//  - VIConfigure / VISetNextFrameBuffer / VISetBlack only take effect at a retrace after VIFlush (the SDK
//    latches the shadow registers when VIFlush is called, and the interrupt handler applies them);
//  - every retrace: pre-retrace callback, latched state applied, retrace count +1, field toggles, post-retrace
//    callback with the new count;
//  - VIWaitForRetrace returns after the next retrace;
//  - VIGetTvFormat follows the configured mode (NTSC 0, PAL 1, MPAL 2, EURGB60 5).
#ifdef RE4_XBOX
#include <dolphin/vi.h>
#include <xbox/xvi_backend.h>
#include <string.h>

extern "C" {

static VIRetraceCallback s_pre, s_post;
static u32 s_retraces, s_field, s_tv, s_scan;
static int s_black, s_curBlack, s_flushedBlack;
static int s_haveMode, s_flushedMode;
static void *s_nextFb, *s_curFb, *s_flushedFb;
static int s_flushedFbValid;
static u16 s_w, s_h, s_flushedW, s_flushedH;
static u32 s_flushedTv, s_flushedScan;
static unsigned long long s_base, s_period;
static int s_inRetrace;
static int s_traceBudget;  // development: how many retraces to describe on the console screen (set by VIFlush)
#ifdef NXDK
#include "xbox/xtrace.h"
#define VI_TRACE(s) XTrace_Add(s "\n")
#define VI_CRUMB(s) XTrace_Crumb(s)
#else
#define VI_TRACE(s)
#define VI_CRUMB(s)
#endif

static unsigned long long PeriodFor(u32 tv)
{
    // 59.94 Hz fields for NTSC / MPAL / EURGB60, 50 Hz for PAL
    return tv == VI_PAL ? 20000ull : 16683ull;
}

static void OneRetrace(void)
{
    int tr = s_traceBudget > 0;
    VI_CRUMB("VI retrace begin");
    if (tr) {
        s_traceBudget--;
        VI_TRACE("VI: retrace begin");
    }
    if (s_pre) {
        s_pre(s_retraces);
    }
    if (s_flushedMode) {  // VIFlush happened since the last retrace: latch it
        s_curFb = s_flushedFbValid ? s_flushedFb : s_curFb;
        s_curBlack = s_flushedBlack;
        s_w = s_flushedW;
        s_h = s_flushedH;
        s_tv = s_flushedTv;
        s_scan = s_flushedScan;
        s_period = PeriodFor(s_tv);
    }
    s_retraces++;
    s_field ^= 1;
    if (tr) {
        VI_TRACE("VI: present");
    }
    VI_CRUMB("VI presenting");
    XViBackend_Present(s_curFb, s_curBlack, s_w, s_h);
    VI_CRUMB("VI post callback");
    if (tr) {
        VI_TRACE("VI: post callback");
    }
    if (s_post) {
        s_post(s_retraces);
    }
    VI_CRUMB("VI retrace end");
    if (tr) {
        VI_TRACE("VI: retrace end");
    }
}

// runs the retraces that have elapsed on the virtual clock; returns how many
static int Advance(void)
{
    unsigned long long now, due;
    int n = 0;
    if (s_inRetrace || !s_period) {
        return 0;
    }
    s_inRetrace = 1;
    now = XViBackend_NowUs();
    due = (now - s_base) / s_period;
    if (due > 8) {  // the game stalled (loading, a debugger): do not replay a burst of callbacks
        s_base = now - 8 * s_period;
        due = 8;
    }
    while (due--) {
        s_base += s_period;
        OneRetrace();
        s_flushedMode = 0;
        n++;
    }
    s_inRetrace = 0;
    return n;
}

void VIInit(void)
{
    s_pre = s_post = 0;
    s_retraces = 0;
    s_field = 0;
    s_tv = VI_NTSC;
    s_scan = 0;
    s_black = 1;
    s_curBlack = 1;
    s_haveMode = 0;
    s_flushedMode = 0;
    s_flushedFbValid = 0;
    s_nextFb = s_curFb = s_flushedFb = 0;
    s_w = 640;
    s_h = 480;
    s_period = PeriodFor(s_tv);
    s_base = XViBackend_NowUs();
    s_inRetrace = 0;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = s_pre;
    s_pre = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = s_post;
    s_post = cb;
    return old;
}

void VIConfigure(const GXRenderModeObj* rm)
{
    s_flushedTv = (u32) rm->viTVmode >> 2;
    s_flushedScan = (u32) rm->viTVmode & 3;
    s_flushedW = rm->fbWidth;
    s_flushedH = rm->xfbHeight;
    s_haveMode = 1;
}

void VIConfigurePan(u16, u16, u16, u16) {}
void VISet3D(BOOL) {}

void VISetNextFrameBuffer(void* fb)
{
    s_nextFb = fb;
}

void VISetNextRightFrameBuffer(void*) {}

void VISetBlack(BOOL black)
{
    s_black = black ? 1 : 0;
}

void VIFlush(void)
{
    s_traceBudget = 4;
    VI_CRUMB("VIFlush enter");
    VI_TRACE("VIFlush: enter");
    Advance();
    VI_CRUMB("VIFlush advanced");
    VI_TRACE("VIFlush: advanced");
    if (!s_haveMode) {  // never configured: keep the current mode
        s_flushedTv = s_tv;
        s_flushedScan = s_scan;
        s_flushedW = s_w;
        s_flushedH = s_h;
    }
    s_flushedFb = s_nextFb;
    s_flushedFbValid = s_nextFb != 0;
    s_flushedBlack = s_black;
    s_flushedMode = 1;
}

void VIWaitForRetrace(void)
{
    u32 start;
    Advance();
    start = s_retraces;
    while (s_retraces == start) {
        XViBackend_SleepUntilUs(s_base + s_period);
        Advance();
    }
}

u32 VIGetRetraceCount(void)
{
    Advance();
    return s_retraces;
}

u32 VIGetNextField(void)
{
    Advance();
    return s_field ^ 1;
}

u32 VIGetCurrentLine(void)
{
    unsigned long long now;
    Advance();
    now = XViBackend_NowUs();
    return (u32) ((now - s_base) * (s_tv == VI_PAL ? 312 : 262) / s_period);
}

u32 VIGetTvFormat(void) { return s_tv; }
u32 VIGetScanMode(void) { return s_scan; }
u32 VIGetDTVStatus(void) { return 0; }  // composite / S-video, not the digital (component) path
void* VIGetNextFrameBuffer(void) { return s_nextFb; }
void* VIGetCurrentFrameBuffer(void) { return s_curFb; }

}
#endif
