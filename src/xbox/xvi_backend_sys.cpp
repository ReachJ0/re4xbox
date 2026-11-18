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

void XViBackend_Present(const void* fb, int black, int, int)
{
    static int state;   // 0 = not started, 1 = running, -1 = pbkit failed: present nothing
    if (state == 0) {
        state = pb_init() == 0 ? 1 : -1;
        if (state > 0) {
            pb_show_front_screen();
        }
    }
    if (state < 0) {
        return;
    }
    int w = (int) pb_back_buffer_width();
    int h = (int) pb_back_buffer_height();
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, w, h);
    pb_fill(0, 0, w, h, (black || !fb) ? 0x00000000 : 0x00102060);
    while (pb_busy()) {
    }
    while (pb_finished()) {   // too early for the swap: wait, this paces the loop to the real retrace
    }
}

}
#endif
