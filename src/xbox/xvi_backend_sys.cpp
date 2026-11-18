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

#define TRACE_LINES 24
#define TRACE_COLS 70
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

void XTrace_Add(const char* text)
{
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

static int s_state;   // 0 = pbkit not started, 1 = running, -1 = pbkit failed: present nothing
static unsigned s_presents;

static void EnsurePb(void)
{
    if (s_state == 0) {
        s_state = pb_init() == 0 ? 1 : -1;
        if (s_state > 0) {
            pb_show_front_screen();
        }
    }
}

static void DrawFrame(const void* fb, int black, const char* status)
{
    int w = (int) pb_back_buffer_width();
    int h = (int) pb_back_buffer_height();
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, w, h);
    pb_fill(0, 0, w, h, (black || !fb) ? 0x00000000 : 0x00102060);
    pb_erase_text_screen();
    pb_print("%s\n", status);
    Lock();
    for (int i = 1; i <= TRACE_LINES; i++) {   // oldest first, the line being written last
        const char* l = s_lines[(s_head + i) % TRACE_LINES];
        if (l[0] || i == TRACE_LINES) {
            pb_print("%s\n", l);
        }
    }
    Unlock();
    pb_draw_text_screen();
    while (pb_busy()) {
    }
    while (pb_finished()) {   // too early for the swap: wait, this paces the loop to the real retrace
    }
}

void XViBackend_Present(const void* fb, int black, int, int)
{
    char status[64];
    EnsurePb();
    if (s_state < 0) {
        return;
    }
    s_presents++;
    snprintf(status, sizeof(status), "present %u black=%d fb=%p", s_presents, black, fb);
    DrawFrame(fb, black, status);
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
