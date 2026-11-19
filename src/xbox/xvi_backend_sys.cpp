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

void XViBackend_Present(const void* fb, int black, int, int)
{
    char status[96];
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
