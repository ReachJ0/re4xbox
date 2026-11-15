// Host unit test for the Xbox thread layer (src/xbox/thread_xbox.cpp + xthread_backend_sys.cpp) running the
// SDK's own scheduler (OSThread.c, OSMutex.c, OSSemaphore.c, unchanged). The task scheduler of the game
// (src/game/scheduler.cpp) is reproduced call for call in TestSchedulerPattern. Real threads underneath.
// Build and run with tools/xbox/run_thread_test.sh.
#include <dolphin/os.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "xbox/xthread_backend.h"
#include "xbox/xos_backend.h"

extern "C" u32 xos_cntlzw(u32);
static int xos_cntlzw_check() { return xos_cntlzw(0) == 32 && xos_cntlzw(1) == 31 && xos_cntlzw(0x80000000u) == 0 && xos_cntlzw(0x00010000u) == 15; }
static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake OS backend --------------------------------------------------------------------------------------
extern "C" {
int XOsBackend_Init(void) { return 1; }
unsigned long long XOsBackend_Nanoseconds(void) { return 0; }
unsigned int XOsBackend_WallSeconds2000(void) { return 0; }
void XOsBackend_Log(const char* t) { fputs(t, stdout); }
void XOsBackend_Halt(void) { printf("HALT\n"); abort(); }
void XOsBackend_Reset(int) { abort(); }
int XOsBackend_ResetButton(void) { return 0; }
void* XOsBackend_AllocMain(unsigned int size) { void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0); return p == MAP_FAILED ? NULL : p; }
}

static char g_log[512]; static int g_n;
static void L(char c) { if (g_n < 511) { g_log[g_n++] = c; g_log[g_n] = 0; } }
static void ResetLog() { g_n = 0; g_log[0] = 0; }

// stacks for the SDK: only their addresses matter, and on this host they must be below 4 GB
static u8* StackMem(int n)
{
    static u8* base = 0;
    if (!base) base = (u8*) mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return base + 0x4000 * (n + 1);   // the top of region n
}

static OSThread* g_main;

// A finished thread gives its platform thread back a moment after it handed the baton on, so the count
// settles shortly after; wait for it (a leak would never settle).
static int LiveSettles(int atMost)
{
    for (int i = 0; i < 2000; i++) {
        if (XThreadBackend_Live() <= atMost) return 1;
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
    return 0;
}

// ---- 1. basics -----------------------------------------------------------------------------------------------
static OSThread T1, T2, T3;
static void* Fa(void*) { L('a'); return (void*) 7; }
static void* Fb(void* p) { L('b'); L((char) ('0' + (intptr_t) p)); return NULL; }
static void TestBasics()
{
    g_main = OSGetCurrentThread();
    CHECK(g_main != NULL && g_main->priority == 16 && g_main->state == OS_THREAD_STATE_RUNNING);
    CHECK(XThreadBackend_Live() == 1);
    // a higher priority thread resumed by main runs at once, main continues when it ends
    ResetLog();
    CHECK(OSCreateThread(&T1, Fa, NULL, StackMem(1), 0x2000, 15, 0));
    CHECK(T1.state == OS_THREAD_STATE_READY && T1.suspend == 1 && *(u32*) T1.stackEnd == OS_THREAD_STACK_MAGIC);
    L('m'); OSResumeThread(&T1); L('M');
    CHECK(strcmp(g_log, "maM") == 0);
    CHECK(T1.state == OS_THREAD_STATE_MORIBUND && OSIsThreadTerminated(&T1));
    void* r = 0; CHECK(OSJoinThread(&T1, &r) == 1 && (intptr_t) r == 7);        // the function's return value
    CHECK(OSIsThreadTerminated(&T1) && LiveSettles(1));
    // a lower priority thread waits until main blocks; detached ones vanish on exit
    ResetLog();
    OSCreateThread(&T2, Fb, (void*) 5, StackMem(2), 0x2000, 20, 1);
    OSResumeThread(&T2); L('m');
    CHECK(strcmp(g_log, "m") == 0 && T2.state == OS_THREAD_STATE_READY);
    OSYieldThread();                                                    // main still higher: nothing changes
    CHECK(strcmp(g_log, "m") == 0);
    OSSetThreadPriority(g_main, 25);                                    // now the other one is better: it runs
    L('M');
    CHECK(strcmp(g_log, "mb5M") == 0 && T2.state == 0);                 // detached and ended
    OSSetThreadPriority(g_main, 16);
    CHECK(LiveSettles(1));
}

// ---- 2. same priority round robin, sleep / wakeup -----------------------------------------------------------------
static OSThreadQueue Q;
static void* Fy(void* p) { char c = (char) (intptr_t) p; for (int i = 0; i < 3; i++) { L(c); OSYieldThread(); } return NULL; }
static void* Fsleep(void*) { L('s'); OSSleepThread(&Q); L('w'); return NULL; }
static void TestYieldAndSleep()
{
    ResetLog();
    OSCreateThread(&T1, Fy, (void*) 'x', StackMem(1), 0x2000, 16, 1);
    OSCreateThread(&T2, Fy, (void*) 'y', StackMem(2), 0x2000, 16, 1);
    OSResumeThread(&T1); OSResumeThread(&T2);                           // same priority as main: queued behind it
    CHECK(g_n == 0);
    for (int i = 0; i < 4; i++) { L('.'); OSYieldThread(); }
    CHECK(strcmp(g_log, ".xy.xy.xy.") == 0);
    while (!OSIsThreadTerminated(&T1) || !OSIsThreadTerminated(&T2)) OSYieldThread();
    // sleep and wakeup
    ResetLog(); OSInitThreadQueue(&Q);
    OSCreateThread(&T3, Fsleep, NULL, StackMem(3), 0x2000, 15, 1);
    OSResumeThread(&T3);                                                // runs at once, sleeps, main continues
    CHECK(strcmp(g_log, "s") == 0 && T3.state == OS_THREAD_STATE_WAITING && Q.head == &T3);
    OSWakeupThread(&Q);                                                 // better priority: runs at once
    CHECK(strcmp(g_log, "sw") == 0 && Q.head == NULL);
    CHECK(LiveSettles(1));
}

// ---- 3. the game's task scheduler, call for call -------------------------------------------------------------------
enum { TASK_NONE = 0, TASK_EXEC = 1, TASK_SLEEP = 2, TASK_RUN = 3 };
struct Task { OSThread Thread; OSThreadQueue Queue; int Status; int SleepCtr; int Priority; void* (*hook)(void*); void (*pFunc)(int); intptr_t arg; u8* pStack; u32 StackSize; };
static Task g_task; static Task* pCTask; static OSThread* pParentThread; static OSSemaphore Sema;
static void TaskSleep(int ctr)
{
    pCTask->SleepCtr = ctr; pCTask->Status = TASK_SLEEP;
    if (pParentThread) OSResumeThread(pParentThread);
    if (pCTask->Priority > 0xF) OSSignalSemaphore(&Sema);
    OSSleepThread(&pCTask->Queue);
    if (pParentThread) OSSuspendThread(pParentThread);
}
static void TaskExit()
{
    pCTask->Status = TASK_NONE;
    if (pParentThread) OSResumeThread(pParentThread);
    if (pCTask->Priority > 0xF) OSSignalSemaphore(&Sema);
    OSExitThread(NULL);
}
static void* TaskExec_hook(void* value)
{
    if (pParentThread) OSSuspendThread(pParentThread);
    pCTask->pFunc((int) (intptr_t) value);
    return NULL;
}
static void TaskBody(int arg) { for (int i = 0; i < 3; i++) { L((char) ('A' + arg)); TaskSleep(2); } L('Z'); TaskExit(); }
static void SchedulerMain(Task* pT)
{
    switch (pT->Status) {
    case TASK_EXEC:
        OSCreateThread(&pT->Thread, pT->hook, (void*) pT->arg, pT->pStack, pT->StackSize, pT->Priority, 1);
        pT->Status = TASK_RUN; OSResumeThread(&pT->Thread); break;
    case TASK_SLEEP:
        if (--pT->SleepCtr != 0) return;
        pT->Status = TASK_RUN; OSWakeupThread(&pT->Queue); break;
    case TASK_RUN: OSResumeThread(&pT->Thread); break;
    default: return;
    }
    if (pCTask->Priority > 0xF) OSWaitSemaphore(&Sema);
}
static void RunTask(int prio, int arg, const char* expect)
{
    ResetLog();
    g_task.Status = TASK_EXEC; g_task.Priority = prio; g_task.hook = TaskExec_hook; g_task.pFunc = TaskBody; g_task.arg = arg;
    g_task.pStack = StackMem(4); g_task.StackSize = 0x2000; OSInitThreadQueue(&g_task.Queue); OSInitSemaphore(&Sema, 0);
    for (int frame = 0; frame < 12; frame++) {
        L('|');
        pParentThread = OSGetCurrentThread(); pCTask = &g_task;
        SchedulerMain(&g_task);
        // main waits for the next frame (vsync): a task of lower priority than main, which was preempted by its
        // own OSSignalSemaphore before it could go to sleep, finishes going to sleep here
        OSSetThreadPriority(OSGetCurrentThread(), 31); OSSetThreadPriority(OSGetCurrentThread(), 16);
    }
    CHECK(strcmp(g_log, expect) == 0);
    if (strcmp(g_log, expect) != 0) printf("   got %s\n   exp %s\n", g_log, expect);
    CHECK(g_task.Status == TASK_NONE && OSIsThreadTerminated(&g_task.Thread) && LiveSettles(1));
}
static void TestSchedulerPattern()
{
    // A task that steps on frames 0, 2, 4, sleeps two frames in between, and finishes on frame 6.
    const char* expect = "|A||A||A||Z|||||";
    RunTask(15, 0, expect);   // priority 0xF: better than main (16), runs at once on resume
    RunTask(20, 1, "|B||B||B||Z|||||");   // priority > 0xF: runs only while main waits on the semaphore
    RunTask(15, 2, "|C||C||C||Z|||||");   // the same slot starts again after it ended
}

// ---- 4. cancel and reuse ----------------------------------------------------------------------------------------
static void* Fpark(void*) { OSSleepThread(&Q); return NULL; }
static void TestCancelAndReuse()
{
    OSInitThreadQueue(&Q);
    int maxLive = 0;
    for (int i = 0; i < 300; i++) {
        OSCreateThread(&T1, Fpark, NULL, StackMem(1), 0x2000, 15, 1);
        OSResumeThread(&T1);                                           // runs, sleeps on Q
        CHECK(T1.state == OS_THREAD_STATE_WAITING);
        OSCancelThread(&T1);                                           // killed while parked
        CHECK(OSIsThreadTerminated(&T1) && Q.head == NULL);
        { int l = XThreadBackend_Live(); if (l > maxLive) maxLive = l; }
    }
    CHECK(maxLive <= 6 && LiveSettles(1));                              // parked threads are released, not accumulated
    // cancel a thread that never ran
    OSCreateThread(&T2, Fa, NULL, StackMem(2), 0x2000, 15, 1);
    OSCancelThread(&T2);
    CHECK(OSIsThreadTerminated(&T2));
    CHECK(LiveSettles(1));
    // the same OSThread object created again after a normal exit runs fine
    for (int i = 0; i < 3; i++) { ResetLog(); OSCreateThread(&T3, Fb, (void*) (intptr_t) i, StackMem(3), 0x2000, 15, 1); OSResumeThread(&T3); CHECK(g_n == 2 && g_log[1] == '0' + i); }
}

// ---- 5. suspend of the running thread from "interrupt" code -------------------------------------------------------------
static void* Fisr(void*)
{
    OSSuspendThread(g_main);         // what the task hook does
    L('1');
    OSResumeThread(g_main);          // the retrace callback (iTaskSuspend) gives the CPU back ...
    OSSuspendThread(&T1);            // ... and suspends the task, which is the running thread: main continues
    L('2');
    return NULL;
}
static void TestSuspendRunning()
{
    ResetLog();
    OSCreateThread(&T1, Fisr, NULL, StackMem(1), 0x2000, 15, 1);
    OSResumeThread(&T1);
    L('M');
    CHECK(T1.suspend == 1 && T1.state == OS_THREAD_STATE_READY && g_main->suspend == 0);
    OSResumeThread(&T1);
    CHECK(strcmp(g_log, "1M2") == 0 && OSIsThreadTerminated(&T1));
}

// ---- 6. mutex, priority inheritance (SDK code on top of the same switching) -----------------------------------------
static OSMutex Mx;
static void* Flock(void*) { OSLockMutex(&Mx); L('l'); OSUnlockMutex(&Mx); return NULL; }
static void TestMutex()
{
    ResetLog(); OSInitMutex(&Mx);
    OSLockMutex(&Mx);
    OSCreateThread(&T1, Flock, NULL, StackMem(1), 0x2000, 10, 1);
    OSResumeThread(&T1);                         // blocks on the mutex main holds; main inherits priority 10
    CHECK(g_n == 0 && g_main->priority == 10);
    OSUnlockMutex(&Mx);                          // T1 gets it and runs at once
    CHECK(strcmp(g_log, "l") == 0 && g_main->priority == 16);
}

// ---- 7. the context calls on their own ------------------------------------------------------------------------------
static void TestContextCalls()
{
    OSContext c;
    memset(&c, 0xFF, sizeof c); c.state = 2; c.mode = 1;
    OSClearContext(&c);                                   // the SDK's "this context is dead / not saved"
    CHECK(c.state == 0 && c.mode == 0);
    OSLoadContext(OSGetCurrentContext());                 // loading the running context changes nothing and returns
    OSInitContext(&c, 0x1234, 0x5678);
    CHECK(c.srr0 == 0x1234 && c.gpr[1] == 0x5678 && c.gpr[3] == 0 && c.state == 0);
    OSClearContext(&c);                                   // never started: nothing to release
    CHECK(OSSaveContext(&c) == 0);
    CHECK(xos_cntlzw_check());
}

int main()
{
    OSInit();
    TestBasics(); TestYieldAndSleep(); TestSchedulerPattern(); TestCancelAndReuse(); TestSuspendRunning(); TestMutex(); TestContextCalls();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
