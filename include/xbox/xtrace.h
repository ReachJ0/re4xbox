// xbox/xtrace.h: development trace of the Xbox build, shown on the screen by the pbkit backend
// (src/xbox/xvi_backend_sys.cpp, nxdk only). Without it a black screen cannot tell "hung" from "running":
// the game's OSReport / OSPanic text and the progress lines of main.cpp are kept in a ring of the last lines
// and drawn over the frame on every retrace.
#ifndef XBOX_XTRACE_H
#define XBOX_XTRACE_H

#ifdef __cplusplus
extern "C" {
#endif

// Appends text to the ring. Partial lines are fine: a line ends at '\n'. Safe from several threads.
void XTrace_Add(const char* text);

// "label a b" as one line (development counters).
void XTrace_Val(const char* label, int a, int b);

// Development heartbeat (main.cpp): frame number plus what the GX processor / rasterizer have done so far.
void XTrace_Beat(int frame);

// A breadcrumb: kept in a separate small ring that is never drawn on the frame. Only the hang watchdog shows it
// (xvi_backend_sys.cpp), on the debug screen, because a hang inside a frame leaves the frame stale.
void XTrace_Crumb(const char* text);

// Line i (0 = oldest, 10 = the one being written) of the trace ring, "" if empty. For the crash report.
const char* XTrace_Line(int i);
#define XTRACE_NLINES 11

// Switches the display to the CPU-drawn text screen of nxdk (pbkit may be showing its own frame).
void XTrace_ShowTextScreen(void);

// Installs the crash report (src/xbox/xcrash_sys.cpp): an unhandled exception prints its code and address.
void XCrash_Install(void);

// Shows the ring forever (the frame keeps being presented) and never returns. OSPanic uses it instead of
// rebooting the console, so the message stays readable.
void XTrace_HaltForever(void);

#ifdef __cplusplus
}
#endif

// A progress line; compiles to nothing outside the Xbox build, so the GameCube code is unchanged.
#ifdef RE4_XBOX
#define RE4_TRACE(s) XTrace_Add(s "\n")
// Per-frame breadcrumbs of the first 4 frames of the main loop (main.cpp). Written inline so no line numbers move.
#define XFRM_DECL int xFrame = 0;
#define XFRM(txt) if (xFrame < 4) { XTrace_Add("fr " #txt "\n"); }
#define XFRM_END xFrame++; if (xFrame % 60 == 0) { XTrace_Beat(xFrame); }
#define XFRMV(txt, a, b) if (xFrame < 4) { XTrace_Val("fr " #txt, (int) (a), (int) (b)); }
#else
#define RE4_TRACE(s)
#endif

#endif
