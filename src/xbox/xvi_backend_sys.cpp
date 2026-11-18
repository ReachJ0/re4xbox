// nxdk backend of the video interface: a clock and a sleep. Presentation is the renderer's job (the NV2A
// back end, not written yet); until then a retrace presents nothing.
// Assumption, not tried on hardware: KeQueryPerformanceCounter / KeQueryPerformanceFrequency are monotonic.
#ifdef NXDK
#include "xbox/xvi_backend.h"

#include <windows.h>
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

void XViBackend_Present(const void*, int, int, int) {}

}
#endif
