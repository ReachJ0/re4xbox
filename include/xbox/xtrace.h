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

// Shows the ring forever (the frame keeps being presented) and never returns. OSPanic uses it instead of
// rebooting the console, so the message stays readable.
void XTrace_HaltForever(void);

#ifdef __cplusplus
}
#endif

#endif
