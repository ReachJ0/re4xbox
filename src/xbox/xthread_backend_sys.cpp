// xbox/xthread_backend_sys: XThreadBackend_* for the console (nxdk) and, with the same file, for a PC.
//
// Every XThread has a "go" event. Transfer(to) sets to's event and waits for its own; exactly one thread is
// ever awake except while a thread is being released (it then only runs the few lines below that end it,
// touching no game state). Ending a thread, whether it is the caller or a parked one, is a longjmp back to
// the base frame of its thread function, which abandons its stack the way the GameCube abandons a cleared
// context.
//
// Assumptions about nxdk, none of them tried on hardware: CreateThread / CreateEventA / SetEvent /
// WaitForSingleObject / CloseHandle from <windows.h> work as on Windows (auto reset events), and the
// requested stack size is honoured. The game's own thread stacks are not used: they are sized for PowerPC
// code (0x1800 to 0x3000 bytes), these are XTHREAD_STACK each.
#ifdef RE4_XBOX

#include "xbox/xthread_backend.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#ifdef NXDK
#include <windows.h>
#else
#include <pthread.h>
#endif

#define XTHREAD_STACK (256 * 1024)

struct XThread {
    void (*entry)(void*);
    void* arg;
    volatile int discard;
    jmp_buf base;
    int hasThread;
#ifdef NXDK
    HANDLE ev;
    HANDLE th;
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int go;
    pthread_t th;
#endif
};

namespace {

XThread* s_current = 0;
volatile int s_live = 0;
#ifndef NXDK
pthread_mutex_t s_liveMu = PTHREAD_MUTEX_INITIALIZER;
#endif

void LiveAdd(int d)
{
#ifdef NXDK
    InterlockedExchangeAdd((LONG volatile*) &s_live, d);
#else
    pthread_mutex_lock(&s_liveMu);
    s_live += d;
    pthread_mutex_unlock(&s_liveMu);
#endif
}

int EvInit(XThread* t)
{
#ifdef NXDK
    t->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return t->ev != NULL;
#else
    t->go = 0;
    return pthread_mutex_init(&t->mu, NULL) == 0 && pthread_cond_init(&t->cv, NULL) == 0;
#endif
}

void EvSet(XThread* t)
{
#ifdef NXDK
    SetEvent(t->ev);
#else
    pthread_mutex_lock(&t->mu);
    t->go = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
#endif
}

void EvWait(XThread* t)
{
#ifdef NXDK
    WaitForSingleObject(t->ev, INFINITE);
#else
    pthread_mutex_lock(&t->mu);
    while (!t->go) {
        pthread_cond_wait(&t->cv, &t->mu);
    }
    t->go = 0;
    pthread_mutex_unlock(&t->mu);
#endif
}

void Free(XThread* t)
{
#ifdef NXDK
    CloseHandle(t->ev);
    if (t->th) {
        CloseHandle(t->th);
    }
#else
    pthread_mutex_destroy(&t->mu);
    pthread_cond_destroy(&t->cv);
    if (t->hasThread) {
        pthread_detach(t->th);
    }
#endif
    free(t);
}

// body of every spawned thread
#ifdef NXDK
DWORD WINAPI Trampoline(LPVOID p)
#else
void* Trampoline(void* p)
#endif
{
    XThread* t = (XThread*) p;

    if (setjmp(t->base) == 0) {
        EvWait(t);  // sleeps until the first transfer (or a discard before it ever ran)
        if (!t->discard) {
            s_current = t;
            t->entry(t->arg);
            abort();  // a task entry must end with OSExitThread, which never returns
        }
    }
    LiveAdd(-1);
    Free(t);
#ifdef NXDK
    return 0;
#else
    return NULL;
#endif
}

}  // namespace

extern "C" {

int XThreadBackend_Init(void)
{
    XThread* t;

    if (s_current) {
        return 1;
    }
    t = (XThread*) calloc(1, sizeof(XThread));
    if (!t || !EvInit(t)) {
        free(t);
        return 0;
    }
    s_current = t;
    LiveAdd(1);
    return 1;
}

XThread* XThreadBackend_Current(void) { return s_current; }

XThread* XThreadBackend_Spawn(void (*entry)(void*), void* arg)
{
    XThread* t = (XThread*) calloc(1, sizeof(XThread));

    if (!t || !EvInit(t)) {
        free(t);
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;
    LiveAdd(1);
#ifdef NXDK
    t->th = CreateThread(NULL, XTHREAD_STACK, Trampoline, t, 0, NULL);
    t->hasThread = t->th != NULL;
    if (!t->hasThread) {
#else
    {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, XTHREAD_STACK);
        t->hasThread = pthread_create(&t->th, &a, Trampoline, t) == 0;
        pthread_attr_destroy(&a);
    }
    if (!t->hasThread) {
#endif
        LiveAdd(-1);
        Free(t);
        return NULL;
    }
    return t;
}

void XThreadBackend_Transfer(XThread* to)
{
    XThread* self = s_current;

    if (to == self) {
        return;
    }
    EvSet(to);
    EvWait(self);
    if (self->discard) {
        longjmp(self->base, 1);  // released while parked
    }
    s_current = self;
}

void XThreadBackend_TransferAndEnd(XThread* to)
{
    XThread* self = s_current;

    EvSet(to);
    longjmp(self->base, 1);  // to the trampoline: releases this thread
}

void XThreadBackend_Discard(XThread* t)
{
    t->discard = 1;
    EvSet(t);  // it wakes inside Transfer (or at its start) and ends itself
}

int XThreadBackend_Live(void)
{
#ifdef NXDK
    return (int) InterlockedCompareExchange((LONG volatile*) &s_live, 0, 0);
#else
    int n;
    pthread_mutex_lock(&s_liveMu);
    n = s_live;
    pthread_mutex_unlock(&s_liveMu);
    return n;
#endif
}

}  // extern "C"

#endif  // RE4_XBOX
