/* OS / VI services the SDK GX code calls, as inert host stubs (the real ones are in os_xbox / thread_xbox). */
#include <stdio.h>
#include <stdlib.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

BOOL OSDisableInterrupts(void) { return 1; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }
OSTime OSGetTime(void) { static OSTime t; return t += 100; }
u32 __OSBusClock = 162000000;
void OSInitThreadQueue(OSThreadQueue* q) { q->head = q->tail = 0; }
void OSSleepThread(OSThreadQueue* q) { fprintf(stderr, "OSSleepThread would block\n"); abort(); }
void OSWakeupThread(OSThreadQueue* q) {}
OSThread* OSGetCurrentThread(void) { return 0; }
s32 OSResumeThread(OSThread* t) { return 0; }
s32 OSSuspendThread(OSThread* t) { return 0; }
void OSClearContext(OSContext* c) {}
void OSSetCurrentContext(OSContext* c) {}
void OSPanic(const char* file, int line, const char* msg, ...) { fprintf(stderr, "OSPanic %s:%d %s\n", file, line, msg); abort(); }
void PPCSync(void) {}
u32 xos_cntlzw(u32 v) { return v ? __builtin_clz(v) : 32; }
u32 VIGetTvFormat(void) { return VI_NTSC; }
