// xbox/xthread_backend.h: what the Xbox thread layer needs from the platform.
//
// The game runs its tasks as OS threads of the Dolphin SDK scheduler (src/lib/OSThread.c, used unchanged by
// the Xbox build). That scheduler decides who runs and switches by saving and loading PowerPC contexts.
// This layer replaces only the switching: every SDK thread gets a real thread of the platform, and exactly
// one of them runs at any time. "Switching" is handing a baton: wake the other thread, park yourself. So the
// order in which game code runs is the SDK's, and no game code ever runs in parallel.
//   src/xbox/thread_xbox.cpp          the SDK context calls on top of this interface
//   src/xbox/xthread_backend_sys.cpp  nxdk threads on the console, pthreads on a PC (same file, #ifdef NXDK)
// Plain C types only, same reason as xpad_backend.h.
#ifndef XBOX_XTHREAD_BACKEND_H
#define XBOX_XTHREAD_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XThread XThread;

// The calling thread becomes the first one ("main"). Returns 1 on success. Call once, from the thread that
// runs the game's main().
int XThreadBackend_Init(void);

// The thread that is running now.
XThread* XThreadBackend_Current(void);

// A new thread that will run entry(arg) on its own stack the first time it is transferred to. Until then
// it sleeps. NULL if the platform has no more threads or memory.
XThread* XThreadBackend_Spawn(void (*entry)(void*), void* arg);

// Runs `to` and parks the caller until some thread transfers back to it. Returns then.
void XThreadBackend_Transfer(XThread* to);

// The caller is finished and will not run again: runs `to`, then the caller's thread and stack are released.
// Does not return.
void XThreadBackend_TransferAndEnd(XThread* to);

// `t` is parked (or never started) and will never run again: releases its thread and stack. Not for the
// running thread. `t` must not be used afterwards.
void XThreadBackend_Discard(XThread* t);

// Number of platform threads alive (the main one included), to see that finished threads are released.
int XThreadBackend_Live(void);

#ifdef __cplusplus
}
#endif

#endif
