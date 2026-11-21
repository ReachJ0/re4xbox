// nxdk backend of the video interface: a clock, a sleep and the screen swap through pbkit.
// The swap only clears the back buffer and flips (black while the game blanks the screen, a dark blue
// placeholder once it unblanks), because nothing renders into the external frame buffer yet: GXCopyDisp
// and the NV2A drawing are not written. The placeholder tells "the game reached VIFlush with the screen on".
// Assumptions, not tried on hardware or an emulator: KeQueryPerformanceCounter / KeQueryPerformanceFrequency
// are monotonic; the video mode was set (XVideoSetMode) before the first retrace; one thread drives VI
// (pbkit is not thread-safe).
#ifdef NXDK
#include "xbox/xvi_backend.h"

#include <windows.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <stdio.h>
#include <string.h>
#include "xbox/xtrace.h"
#include "xbox/xraster.h"
#include <xboxkrnl/xboxkrnl.h>

extern "C" {

unsigned long long XViBackend_NowUs(void)
{
    static unsigned long long freq;
    unsigned long long now = (unsigned long long) KeQueryPerformanceCounter();
    if (!freq) {
        freq = (unsigned long long) KeQueryPerformanceFrequency();
    }
    return now / freq * 1000000ull + (now % freq) * 1000000ull / freq;
}

void XViBackend_SleepUntilUs(unsigned long long t)
{
    unsigned long long now = XViBackend_NowUs();
    if (t > now) {
        unsigned long long ms = (t - now) / 1000;
        Sleep(ms ? (DWORD) ms : 1);
    }
}

// ---- development trace (xbox/xtrace.h) ---------------------------------------------------------------------

#define TRACE_LINES 11
#define TRACE_COLS 50
static char s_lines[TRACE_LINES][TRACE_COLS + 1];
static int s_head;                       // index of the line being written
static int s_col;
static volatile LONG s_lock;

static void Lock(void)
{
    while (InterlockedExchange(&s_lock, 1)) {
    }
}
static void Unlock(void) { InterlockedExchange(&s_lock, 0); }

static volatile LONG s_traceSeq;   // bumped by every XTrace_Add: the watchdog redraws when it changes

void XTrace_Val(const char* label, int a, int b)
{
    char t[48];
    snprintf(t, sizeof(t), "%s %d %d\n", label, a, b);
    XTrace_Add(t);
}

void XTrace_Add(const char* text)
{
    InterlockedIncrement(&s_traceSeq);
#ifdef RE4_DEBUGSCREEN
    Lock();
    debugPrint("%s", text);   // CPU-drawn text screen of nxdk: no GPU, no pbkit (make DEBUGSCREEN=1)
    Unlock();
#endif
    Lock();
    for (; *text; text++) {
        if (*text == '\n' || s_col >= TRACE_COLS) {
            s_head = (s_head + 1) % TRACE_LINES;
            s_lines[s_head][0] = 0;
            s_col = 0;
            if (*text == '\n') {
                continue;
            }
        }
        if (*text != '\r') {
            s_lines[s_head][s_col++] = *text;
            s_lines[s_head][s_col] = 0;
        }
    }
    Unlock();
}

// ---- breadcrumbs and hang watchdog -------------------------------------------------------------------------

#define CRUMBS 16
static char s_crumb[CRUMBS][48];
static volatile LONG s_crumbN;

void XTrace_Crumb(const char* text)
{
    LONG i = InterlockedIncrement(&s_crumbN) % CRUMBS;
    strncpy(s_crumb[i], text, sizeof(s_crumb[i]) - 1);
    s_crumb[i][sizeof(s_crumb[i]) - 1] = 0;
}

static unsigned s_presents;
static volatile LONG s_pbBusy;               // pbkit is not thread safe: one thread draws at a time
static const void* s_lastFb;
static int s_lastBlack = 1;
static LONG s_drawnSeq = -1;
static void DrawFrame(const void* fb, int black, const char* status);
static int s_state;                          // 0 = pbkit not started, 1 = running, -1 = pbkit failed

// Redraws the screen when the trace changed. The screen only changes when a frame is presented, and the game
// may stop presenting (waiting for the disc, say) with new trace lines nobody could see.
static DWORD WINAPI Watchdog(LPVOID)
{
    for (;;) {
        Sleep(250);
        if (s_state > 0 && s_traceSeq != s_drawnSeq && InterlockedExchange(&s_pbBusy, 1) == 0) {
            char st[64];
            s_drawnSeq = s_traceSeq;
            snprintf(st, sizeof(st), "present %u (trace refresh)", s_presents);
            DrawFrame(s_lastFb, s_lastBlack, st);
            InterlockedExchange(&s_pbBusy, 0);
        }
    }
    return 0;
}

static unsigned s_busyTimeouts, s_finishTimeouts;   // waits on pbkit that gave up (see DrawFrame)

static void EnsurePb(void)
{
    if (s_state == 0) {
#ifdef RE4_DEBUGSCREEN
        s_state = -1;   // pbkit never starts: the text screen of nxdk stays on the display
        return;
#endif
        s_state = pb_init() == 0 ? 1 : -1;
        if (s_state > 0) {
            pb_show_front_screen();
            CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
        }
    }
}

static void DrawFrame(const void* fb, int black, const char* status)
{
    int w = (int) pb_back_buffer_width();
    int h = (int) pb_back_buffer_height();
    XTrace_Crumb("DF reset");
    pb_reset();
    pb_target_back_buffer();
    XTrace_Crumb("DF erase+fill");
    pb_erase_depth_stencil_buffer(0, 0, w, h);
    pb_fill(0, 0, w, h, (black || !fb) ? 0x00000000 : 0x00102060);
    XTrace_Crumb("DF text");
    pb_erase_text_screen();
    Lock();
    for (int i = 1; i <= TRACE_LINES; i++) {   // oldest first, the line being written last
        const char* l = s_lines[(s_head + i) % TRACE_LINES];
        if (l[0] || i == TRACE_LINES) {
            pb_print("%s\n", l);
        }
    }
    Unlock();
    for (int i = 3; i >= 0; i--) {   // the four newest breadcrumbs
        const char* c = s_crumb[(s_crumbN - i + CRUMBS) % CRUMBS];
        if (c[0]) {
            pb_print("~ %s\n", c);
        }
    }
    pb_print("%s\n", status);   // last row: it keeps changing while the game is alive
    XTrace_Crumb("DF draw text");
    pb_draw_text_screen();
    XTrace_Crumb("DF wait busy");
    // Both waits give up after 200 ms (a frame is 17 ms): a stuck GPU would otherwise freeze the whole game
    // with nothing on screen to say why. The counts are in the status line.
    DWORD t0 = GetTickCount();
    while (pb_busy()) {
        if (GetTickCount() - t0 > 200) {
            s_busyTimeouts++;
            break;
        }
    }
    XTrace_Crumb("DF wait swap");
    t0 = GetTickCount();
    while (pb_finished()) {   // too early for the swap: wait, this paces the loop to the real retrace
        if (GetTickCount() - t0 > 200) {
            s_finishTimeouts++;
            break;
        }
    }
    XTrace_Crumb("DF done");
}

#ifdef RE4_DEBUGSCREEN
// With the text screen (no pbkit) the XFB (YUV422, as GXCopyDisp wrote it) is converted straight into the frame buffer
// of nxdk's video mode (32 or 16 bit), centred, whenever a new copy happened. Debug text printed afterwards stays on top.
static void BlitXfb(const unsigned char* xfb, int w, int h)
{
    VIDEO_MODE vm = XVideoGetMode();
    unsigned char* dst = XVideoGetFB();
    int bytes = (vm.bpp + 7) / 8;
    if (!dst || !xfb || (bytes != 4 && bytes != 2) || w <= 0 || h <= 0) return;
    int ox = (vm.width - w) / 2, oy = (vm.height - h) / 2;
    for (int y = 0; y < h; y++) {
        int dy = y + oy;
        if (dy < 0 || dy >= vm.height) continue;
        const unsigned char* s = xfb + y * w * 2;
        for (int x = 0; x < w; x += 2, s += 4) {
            int c = 298 * (s[0] - 16), d = s[1] - 128, e = s[3] - 128;
            for (int k = 0; k < 2; k++) {
                int yy = k ? 298 * (s[2] - 16) : c;
                int r = (yy + 409 * e + 128) >> 8, g = (yy - 100 * d - 208 * e + 128) >> 8, b = (yy + 516 * d + 128) >> 8;
                int dx = x + k + ox;
                if (dx < 0 || dx >= vm.width) continue;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                unsigned char* p = dst + ((size_t) dy * vm.width + dx) * bytes;
                if (bytes == 4) *(unsigned*) p = 0xFF000000u | (unsigned) (r << 16 | g << 8 | b);
                else *(unsigned short*) p = (unsigned short) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
}
#endif

void XViBackend_Present(const void* fb, int black, int w, int h)
{
    char status[96];
#ifdef RE4_DEBUGSCREEN
    static unsigned s_shownSerial = 0xFFFFFFFFu;
    static const void* s_shownFb;
    if (!black && fb && (XRaster_CopySerial() != s_shownSerial || fb != s_shownFb)) {
        s_shownSerial = XRaster_CopySerial();
        s_shownFb = fb;
        if (XRaster_CopySerial()) BlitXfb((const unsigned char*) fb, w, h);
    }
#else
    (void) w;
    (void) h;
#endif
    EnsurePb();
    if (s_state < 0) {
        return;
    }
    while (InterlockedExchange(&s_pbBusy, 1)) {
        Sleep(1);
    }
    s_presents++;
    s_lastFb = fb;
    s_lastBlack = black;
    s_drawnSeq = s_traceSeq;
    snprintf(status, sizeof(status), "present %u black=%d busyTO=%u finTO=%u", s_presents, black, s_busyTimeouts,
             s_finishTimeouts);
    DrawFrame(fb, black, status);
    InterlockedExchange(&s_pbBusy, 0);
}

const char* XTrace_Line(int i)
{
    if (i < 0 || i >= TRACE_LINES) return "";
    return s_lines[(s_head + 1 + i) % TRACE_LINES];
}

void XTrace_ShowTextScreen(void)
{
    if (s_state > 0) {
        pb_show_debug_screen();
    }
}

void XTrace_HaltForever(void)
{
    EnsurePb();
    for (;;) {
        if (s_state < 0) {
            Sleep(1000);
        } else {
            DrawFrame(0, 1, "HALTED");
        }
    }
}

}
#endif
