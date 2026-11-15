// xbox/xos_backend.h: the only thing the Xbox OS layer needs from the platform.
//
// src/xbox/os_xbox.cpp implements the Dolphin OS calls the game makes outside threads (clock and calendar,
// interrupt flag, OSReport / OSPanic, arena, sound and video mode, reset). A backend supplies the few real
// facts underneath: a monotonic clock, the wall clock, somewhere to print, the main memory block, and the
// console reboot. It knows nothing about the game. Two backends:
//   src/xbox/xos_backend_sys.cpp   nxdk on the console, POSIX on a PC (same file, #ifdef NXDK)
//   tools/xbox/os_test.cpp         fake with a settable clock, used by the host unit test
// Plain C types only, same reason as xpad_backend.h.
#ifndef XBOX_XOS_BACKEND_H
#define XBOX_XOS_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

// One-time start. Returns 1 on success.
int XOsBackend_Init(void);

// Nanoseconds from any fixed starting point, never going backwards.
unsigned long long XOsBackend_Nanoseconds(void);

// Seconds since 2000-01-01 00:00:00 of the real-time clock (what the GameCube's RTC counts), 0 if unknown.
unsigned int XOsBackend_WallSeconds2000(void);

// Text for the developer (OSReport). May be called with partial lines; no newline is added.
void XOsBackend_Log(const char* text);

// Called by OSPanic after it logged the message. Must not return.
void XOsBackend_Halt(void);

// Called by OSResetSystem. `kind` is 0 to start the game again, 1 to go back to the console's dashboard (also for a shutdown).
// Must not return.
void XOsBackend_Reset(int kind);

// 1 while the player holds the console's reset gesture, else 0.
int XOsBackend_ResetButton(void);

// A block of `size` bytes aligned to 32 for the game's "main memory" (heaps are carved from it), or NULL.
void* XOsBackend_AllocMain(unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
