// xbox/xdvd.h: what the Xbox disc layer (dvd_xbox.cpp, ar_xbox.cpp) offers besides the Dolphin calls.
//
// The GameCube finishes a disc read or an ARAM transfer later, from an interrupt, and calls the game back
// from there. The game depends on that: e.g. snd_str2 does `DVDReadAsyncPrio(...); str->dvd_busy = 1;`, so
// a callback that ran before DVDReadAsyncPrio returned would clear the flag first and leave the stream
// waiting forever. So this layer never calls back inside the call that started the work. It queues the
// work and runs it, and the callbacks, from XDvd_Pump(). Whoever emulates the console's interrupts calls
// the pump (the OS layer: once per video frame / timer tick), and so does the game wherever it spins
// waiting for such a completion (two places, patched under RE4_XBOX: cAram::DmaTrans in dvd.cpp and
// zero_buff_clear in snd_main.cpp). DVDGetDriveStatus also pumps, because the game polls it in a loop.
#ifndef XBOX_XDVD_H
#define XBOX_XDVD_H

#ifdef __cplusplus
extern "C" {
#endif

// ARAM is emulated as a plain buffer of this size (the GameCube's 16 MB).
#define XARAM_SIZE 0x01000000u

// Runs queued disc commands and ARAM requests, with their callbacks, until nothing is left or a safety
// limit is hit. Safe to call from inside a callback (the spin loops do).
void XDvd_Pump(void);

// 1 if a disc command or an ARAM request is still queued.
int XDvd_Pending(void);

// Calls the game made that this layer had to refuse (bad alignment, out of range, table full, ...). The
// SDK would have stopped in OSPanic; here the call is ignored and counted. 0 in a healthy run.
unsigned int XDvd_ErrorCount(void);

// Forgets everything (queues, entry table, disc id, ARAM). For tests; not needed in the game.
void XDvd_Reset(void);

// The ARAM buffer (allocated on first use), XARAM_SIZE bytes; NULL if the allocation failed. For tests.
unsigned char* XAram_Memory(void);

// Internal, between the two files of the layer.
int XAram_Service(void);  // completes one ARQ request; 1 if there was one
int XAram_Pending(void);
void XAram_Reset(void);
void XDvd_CountError(void);

#ifdef __cplusplus
}
#endif

#endif
