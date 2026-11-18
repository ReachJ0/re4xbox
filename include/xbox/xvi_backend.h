// xbox/xvi_backend.h: what the Xbox video-interface layer needs from the platform.
//
// src/xbox/vi_xbox.cpp implements the Dolphin VI calls the game makes (VIInit, VIConfigure, VIFlush,
// VIWaitForRetrace, VISetNextFrameBuffer, VISetPostRetraceCallback, ...). The console's retrace is a 59.94 /
// 50 Hz interrupt; here it is a virtual clock: the layer asks the backend for the time and runs the retraces
// that have elapsed whenever the game calls into VI (and while it waits for one). A backend provides:
//   src/xbox/xvi_backend_sys.cpp   nxdk: performance counter, Sleep, and the presentation hook
//   tools/xbox/vi_test.cpp         fake clock used by the host unit test
// Plain C types only.
#ifndef XBOX_XVI_BACKEND_H
#define XBOX_XVI_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic microseconds.
unsigned long long XViBackend_NowUs(void);
// Block the calling thread until NowUs() >= t (may return early; the layer re-checks).
void XViBackend_SleepUntilUs(unsigned long long t);
// A retrace happened: `xfb` is the frame buffer the video encoder now scans out (game memory, YUV422 as copied
// by GXCopyDisp), or null when nothing was ever set; `black` is the blanking flag; width/height are the
// external frame buffer's size.
void XViBackend_Present(const void* xfb, int black, int width, int height);

#ifdef __cplusplus
}
#endif
#endif
