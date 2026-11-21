// xbox/thread_xbox: the part of the Dolphin SDK thread library that only the PowerPC can do, for the Xbox.
//
// The SDK scheduler (src/lib/OSThread.c, OSMutex.c, OSSemaphore.c) is plain C and is linked UNCHANGED, built
// with these renames (the build scripts do it, see tools/xbox/run_thread_test.sh):
//   -D__cntlzw=xos_cntlzw
// What it expects from the CPU is a handful of context calls, provided here:
//   OSInitContext OSSaveContext OSLoadContext OSClearContext OSGetCurrentContext OSSetCurrentContext
//   OSGetStackPointer
// plus the globals it keeps in GameCube low memory (__OSCurrentThread, __OSActiveThreadQueue, __gUnkThread1).
//
// A context here is only a name: the OSContext inside an OSThread is the key of a table entry that owns a
// platform thread (xbox/xthread_backend.h). OSLoadContext(ctx) is "run that thread now": it hands the baton
// to it and parks the caller, which resumes when someone loads its own context. The SDK only calls it from
// SelectThread, with the caller about to stop being the running thread, so the order of execution is the
// SDK scheduler's. A thread is created on the first load of its context: it runs func(param) from the context
// (srr0, gpr[3], filled by OSInitContext / OSCreateThread) and then OSExitThread(result), which is what the
// SDK's link register trick (lr = OSExitThread) does on the GameCube.
//
// OSClearContext is "this context is dead": a thread that clears its own (OSExitThread) is released when it
// hands the baton on; one that is cleared while parked (OSCancelThread) is released at once.
// OSSaveContext returns 0 and never "returns a second time": SelectThread only needs that for the
// exception path, which does not exist here.
//
// Not covered: idle waiting. When no thread is ready the SDK spins until an interrupt makes one ready; here
// nothing does that, so it would spin forever. The game never leaves every thread blocked.
// The game's own stacks (OSCreateThread's stack argument) are not used to run; the guard word the SDK writes
// at their end is still written, so the game's overflow check keeps passing.
//
// Only compiled for the Xbox build (RE4_XBOX).
#ifdef RE4_XBOX

#include <dolphin/os.h>

#include <stdint.h>
#include <string.h>

#include "xbox/xthread_backend.h"
#include "xbox/xtrace.h"

// The host tests do not link the trace; the Xbox build overrides this empty one with the real XTrace_Mark.
extern "C" __attribute__((weak)) void XTrace_Mark(int) {}

#define MAX_CONTEXTS 64

extern "C" {
void __OSThreadInit(void);
}

namespace {

struct Entry {
    OSContext* ctx;
    XThread* th;
    int dying;  // its thread cleared its own context and is about to hand the baton on for good
};

Entry s_tab[MAX_CONTEXTS];
OSContext* s_cur = 0;
int s_bootPending = 0;
u32 s_fakeSp = 0;  // see OSGetStackPointer

Entry* Find(const OSContext* c)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (s_tab[i].ctx == c) {
            return &s_tab[i];
        }
    }
    return 0;
}

Entry* FindByThread(const XThread* t)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (s_tab[i].ctx && s_tab[i].th == t) {
            return &s_tab[i];
        }
    }
    return 0;
}

Entry* Add(OSContext* c)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!s_tab[i].ctx) {
            s_tab[i].ctx = c;
            s_tab[i].th = 0;
            s_tab[i].dying = 0;
            return &s_tab[i];
        }
    }
    OSPanic(__FILE__, __LINE__, "thread layer: no room for another context");
    return 0;
}

void Remove(Entry* e)
{
    e->ctx = 0;
    e->th = 0;
    e->dying = 0;
}

// first code of every thread but the main one
void ThreadMain(void* arg)
{
    Entry* e = (Entry*) arg;
    void* (*func)(void*) = (void* (*) (void*) )(uintptr_t) e->ctx->srr0;
    void* param = (void*) (uintptr_t) e->ctx->gpr[3];

    OSExitThread(func(param));
}

}  // namespace

extern "C" {

// ---- globals the SDK keeps in low memory ----------------------------------------------------------------

OSThreadQueue __OSActiveThreadQueue;
OSThread* __OSCurrentThread;
OSThread* __gUnkThread1;
u32 __OSFpscrEnableBits;
u8 _stack_addr[64];
u8 _stack_end[64];

u32 xos_cntlzw(u32 v) { return v == 0 ? 32 : (u32) __builtin_clz(v); }

// Only OSClearStack asks, once, while the SDK starts: it paints the stack of the main thread from its end up to
// "here". The SDK's idea of that stack is the two dummy arrays above, so during start-up the answer is a point
// inside them, and the painting stays inside them. Later it is the real stack pointer.
u32 OSGetStackPointer(void) { return s_fakeSp ? s_fakeSp : (u32) (uintptr_t) __builtin_frame_address(0); }

// ---- thread layer set-up ---------------------------------------------------------------------------------

// Called by OSInit: the caller becomes the main thread and the SDK scheduler starts.
void XOs_ThreadInit(void)
{
    XThreadBackend_Init();
    s_bootPending = 1;
    s_fakeSp = (u32) (uintptr_t) (_stack_end + 32);
    __OSThreadInit();
    s_fakeSp = 0;
}

// ---- contexts ------------------------------------------------------------------------------------------

void OSInitContext(OSContext* ctx, u32 pc, u32 newsp)
{
    Entry* e = Find(ctx);

    if (e) {  // a context being reused: whatever ran in it is dead
        if (e->th && e->th != XThreadBackend_Current()) {
            XThreadBackend_Discard(e->th);
        }
        Remove(e);
    }
    memset(ctx, 0, sizeof(OSContext));
    ctx->srr0 = pc;
    ctx->gpr[1] = newsp;
    Add(ctx);
}

void OSClearContext(OSContext* ctx)
{
    Entry* e = Find(ctx);

    ctx->mode = 0;
    ctx->state = 0;
    if (!e) {
        return;
    }
    if (!e->th) {
        Remove(e);
    } else if (e->th == XThreadBackend_Current()) {
        e->dying = 1;  // released when it hands the baton on
    } else {
        XThreadBackend_Discard(e->th);
        Remove(e);
    }
}

u32 OSSaveContext(OSContext*) { return 0; }

OSContext* OSGetCurrentContext(void) { return s_cur; }

void OSSetCurrentContext(OSContext* ctx)
{
    s_cur = ctx;
    if (s_bootPending && ctx && !Find(ctx)) {  // the SDK adopts the booting thread's context
        Add(ctx)->th = XThreadBackend_Current();
        s_bootPending = 0;
    }
}

void OSLoadContext(OSContext* ctx)
{
    Entry* to = Find(ctx);
    Entry* me = FindByThread(XThreadBackend_Current());

    if (!to) {
        OSPanic(__FILE__, __LINE__, "OSLoadContext(): unknown context");
    }
    if (!to->th) {
        to->th = XThreadBackend_Spawn(ThreadMain, to);
        if (!to->th) {
            OSPanic(__FILE__, __LINE__, "OSLoadContext(): no thread for the new context");
        }
    }
    if (to->th == XThreadBackend_Current()) {
        return;
    }
    if (me && !me->dying) {
        XTrace_Mark(10);
        XThreadBackend_Transfer(to->th);  // parked here until our own context is loaded again
        XTrace_Mark(11);
        return;
    }
    if (me) {
        Remove(me);
    }
    XThreadBackend_TransferAndEnd(to->th);
}

}  // extern "C"

#endif  // RE4_XBOX
