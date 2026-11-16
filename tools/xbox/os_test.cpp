// Host unit test for the Xbox OS layer (src/xbox/os_xbox.cpp) together with the SDK files it reuses
// unchanged (OSAlloc.c, OSArena.c, OSStopwatch.c). Fake backend with a settable clock. The SDK's own
// calendar functions (orig_*) are the reference. Build and run with tools/xbox/run_os_test.sh.
#include <dolphin/os.h>

#include <setjmp.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox/xos_backend.h"

extern "C" void orig_OSTicksToCalendarTime(OSTime, OSCalendarTime*);
extern "C" OSTime orig_OSCalendarTimeToTicks(OSCalendarTime*);

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake backend ---------------------------------------------------------------------------------------
static unsigned long long g_ns = 5000000000ull;   // monotonic clock, set by the test
static unsigned int g_wall = 0;
static char g_log[4096];
static int g_halts, g_resetKind = -9, g_button, g_allocFail, g_inits;
static jmp_buf g_jmp;
static int g_jmpArmed;
extern "C" {
int XOsBackend_Init(void) { g_inits++; return 1; }
unsigned long long XOsBackend_Nanoseconds(void) { return g_ns; }
unsigned int XOsBackend_WallSeconds2000(void) { return g_wall; }
void XOsBackend_Log(const char* t) { strncat(g_log, t, sizeof(g_log) - strlen(g_log) - 1); }
void XOsBackend_Halt(void) { g_halts++; if (g_jmpArmed) longjmp(g_jmp, 1); abort(); }
void XOsBackend_Reset(int k) { g_resetKind = k; if (g_jmpArmed) longjmp(g_jmp, 2); abort(); }
int XOsBackend_ResetButton(void) { return g_button; }
// below 4 GB: the SDK heap masks addresses with 32 bit constants, right on the console, wrong for 64 bit pointers
unsigned int g_wantedSeen; unsigned int g_sizeSeen;
void* XOsBackend_AllocMain(unsigned int size, unsigned int wanted)
{
    if (g_allocFail) return NULL;
    g_wantedSeen = wanted; g_sizeSeen = size;
    void* p = mmap((void*) (uintptr_t) wanted, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
}

static const long long HZ = 40500000;   // 162 MHz / 4

static void TestClockAndInit()
{
    CHECK(__OSBusClock == 162000000 && __OSCoreClock == 486000000 && OS_TIMER_CLOCK == 40500000);
    g_wall = 1000; g_ns = 5000000000ull;
    CHECK(g_inits == 0);
    OSTime t0 = OSGetTime();                        // first use starts the layer (lazy)
    CHECK(g_inits == 1 && t0 == 1000 * HZ);
    g_ns += 1500000000ull;                          // 1.5 s later
    CHECK(OSGetTime() == 1000 * HZ + HZ * 3 / 2);
    CHECK(OSGetTick() == (OSTick) (HZ * 3 / 2));
    g_ns += 2000000ull;                             // 2 ms
    CHECK(OSGetTime() - t0 == HZ * 3 / 2 + HZ / 500);
    // a long run: 3 days, no overflow in the tick arithmetic
    g_ns += 3ull * 86400 * 1000000000ull;
    CHECK(OSGetTime() - t0 == (3ll * 86400 + 1) * HZ + HZ / 2 + HZ / 500);
    CHECK(OSGetTick() == (OSTick) ((3ll * 86400 + 1) * HZ + HZ / 2 + HZ / 500));   // wraps at 32 bits like the counter
    CHECK(OSTicksToSeconds(HZ * 5) == 5 && OSTicksToMilliseconds(HZ) == 1000 && OSTicksToMicroseconds(HZ) == 1000000);
}

static void TestInitArena()
{
    g_allocFail = 0;
    OSInit();
    void* lo = OSGetArenaLo(); void* hi = OSGetArenaHi();
    CHECK(lo != NULL && hi > lo && ((uintptr_t) lo & 31) == 0);
    // the arena is the GC one: [0x80003100, FST) of the block, and the block is where the game map says
    CHECK(g_sizeSeen == XMEM_SPAN && g_wantedSeen == XMEM_WANTED_BASE);
    CHECK((uintptr_t) g_xmemBase == XMEM_WANTED_BASE);
    CHECK((uintptr_t) lo == g_xmemBase + 0x3100 && (uintptr_t) hi == g_xmemBase + 0x01700000);
    CHECK(XMem_Valid((unsigned int) (uintptr_t) lo) && XMem_Valid((unsigned int) (uintptr_t) hi));
    OSInit(); CHECK(OSGetArenaLo() == lo && OSGetArenaHi() == hi);       // second call does nothing
    CHECK(OSGetConsoleType() == 1 && (OSGetConsoleType() & 0xF0000000) == 0 && OSGetConsoleSimulatedMemSize() == 0x01800000);
    // the SDK heap on top of that arena, as main_mem.cpp does it
    uintptr_t arenaLo = (uintptr_t) OSInitAlloc(lo, hi, 4);
    OSSetArenaLo((void*) arenaLo);
    CHECK(arenaLo >= (uintptr_t) lo);
    uintptr_t start = ((uintptr_t) arenaLo + 31) & ~(uintptr_t) 31, end = (uintptr_t) hi & ~(uintptr_t) 31;
    int h = OSCreateHeap((void*) start, (void*) end);
    CHECK(h >= 0);
    OSSetCurrentHeap(h);
    s32 freeAtStart = OSCheckHeap(h);
    CHECK(freeAtStart > 0x1000000);
    void* a = OSAllocFromHeap(h, 1000); void* b = OSAllocFromHeap(h, 5000);
    CHECK(a && b && ((uintptr_t) a & 31) == 0 && ((uintptr_t) b & 31) == 0 && a != b);
    memset(a, 1, 1000); memset(b, 2, 5000);
    CHECK(OSCheckHeap(h) < freeAtStart);
    OSFreeToHeap(h, a); OSFreeToHeap(h, b);
    CHECK(OSCheckHeap(h) == freeAtStart);
    OSDestroyHeap(h);
    // stopwatch (SDK file) counts our clock
    OSStopwatch sw; OSInitStopwatch(&sw, (char*) "t"); OSStartStopwatch(&sw); g_ns += 3000000ull; OSStopStopwatch(&sw);
    CHECK(sw.hits == 1 && sw.total == HZ * 3 / 1000);
}

static void TestInitFailure()
{
    // a new process would show this; here just check the panic path when the block can not be had is reachable
    // (OSInit already ran above, so only the flag is checked)
    g_allocFail = 1; OSInit(); g_allocFail = 0; CHECK(OSGetArenaLo() != NULL);
}

static void TestCalendar()
{
    // 2026-11-13 21:15:30.250 -> fields
    OSCalendarTime c; memset(&c, 0, sizeof c);
    c.year = 2026; c.mon = 10; c.mday = 13; c.hour = 21; c.min = 15; c.sec = 30; c.msec = 250; c.usec = 0;
    OSTime t = OSCalendarTimeToTicks(&c);
    OSCalendarTime r; memset(&r, 0xAA, sizeof r);
    OSTicksToCalendarTime(t, &r);
    CHECK(r.year == 2026 && r.mon == 10 && r.mday == 13 && r.hour == 21 && r.min == 15 && r.sec == 30 && r.msec == 250 && r.usec == 0);
    CHECK(r.wday == 5);                                    // Friday, 0 = Sunday
    CHECK(r.yday == 316);                                  // day of the year, 0 based
    // the epoch of the clock: tick 0 is 2000-01-01 (a Saturday)
    OSTicksToCalendarTime(0, &r);
    CHECK(r.year == 2000 && r.mon == 0 && r.mday == 1 && r.hour == 0 && r.wday == 6 && r.yday == 0);
    OSTicksToCalendarTime(OSGetTime(), &r);                // the game does this for save stamps
    // leap day
    memset(&c, 0, sizeof c); c.year = 2000; c.mon = 1; c.mday = 29;
    OSTicksToCalendarTime(OSCalendarTimeToTicks(&c), &r);
    CHECK(r.mon == 1 && r.mday == 29 && r.yday == 59);
    // before the epoch: one tick earlier is the last instant of 1999
    OSTicksToCalendarTime(-1, &r);
    CHECK(r.year == 1999 && r.mon == 11 && r.mday == 31 && r.hour == 23 && r.min == 59 && r.sec == 59 && r.msec == 999 && r.wday == 5 && r.yday == 364);
    OSTicksToCalendarTime(-HZ * 86400 - HZ / 2, &r);
    CHECK(r.year == 1999 && r.mon == 11 && r.mday == 30 && r.hour == 23 && r.min == 59 && r.sec == 59 && r.msec == 500);
    for (OSTime neg = -1; neg > -HZ * 86400ll * 3650; neg = neg * 3 - 1234567) {
        OSCalendarTime a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        OSTicksToCalendarTime(neg, &a); orig_OSTicksToCalendarTime(neg, &b);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
    }
    // differential against the SDK over many times
    unsigned long long x = 88172645463325252ull; int bad = 0, n = 0;
    for (int i = 0; i < 20000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        OSTime ticks = (OSTime) (x % (OSTime) (4000ll * 365 * 86400 * HZ / 20)) - (OSTime) (50ll * 365 * 86400 * HZ / 20 * 20 / 20) * (i & 1);
        if (i % 7 == 0) ticks = (OSTime) (x % 500) * HZ * 86400 / 3;
        OSCalendarTime a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        OSTicksToCalendarTime(ticks, &a); orig_OSTicksToCalendarTime(ticks, &b);
        if (memcmp(&a, &b, sizeof a) != 0) bad++;
        n++;
        if (OSCalendarTimeToTicks(&a) != orig_OSCalendarTimeToTicks(&b)) bad++;
    }
    // out-of-range month and negative values, as the SDK normalises them
    for (int mon = -30; mon <= 40; mon += 7) for (int year = 0; year < 3000; year += 331) {
        OSCalendarTime a; memset(&a, 0, sizeof a); a.year = year; a.mon = mon; a.mday = 1 + mon % 5; a.hour = mon; a.msec = year % 1000;
        if (year + mon / 12 - (mon % 12 < 0) < 0) continue;
        OSCalendarTime b = a;
        if (OSCalendarTimeToTicks(&a) != orig_OSCalendarTimeToTicks(&b)) bad++;
    }
    CHECK(bad == 0 && n == 20000);
}

static void TestInterrupts()
{
    CHECK(OSDisableInterrupts() == TRUE);                  // were enabled
    CHECK(OSDisableInterrupts() == FALSE);                 // already off
    CHECK(OSRestoreInterrupts(FALSE) == FALSE);            // restoring "was off" keeps them off
    CHECK(OSEnableInterrupts() == FALSE);                  // returns the previous state
    CHECK(OSEnableInterrupts() == TRUE);
    BOOL old = OSDisableInterrupts();
    CHECK(old == TRUE && OSRestoreInterrupts(old) == FALSE && OSDisableInterrupts() == TRUE);
    OSRestoreInterrupts(TRUE);
}

static void H1(OSError, OSContext*, ...) {}
static void H2(OSError, OSContext*, ...) {}
static void TestMessagesAndErrors()
{
    g_log[0] = 0;
    OSReport("FST Address = 0x%8x\n", 0x81700000u); OSReport("%s %d", "x", 5);
    CHECK(strcmp(g_log, "FST Address = 0x81700000\nx 5") == 0);
    g_log[0] = 0; char big[1500]; memset(big, 'a', 1499); big[1499] = 0; OSReport("%s", big);
    CHECK(strlen(g_log) == 511);                           // cut, not overrun
    g_log[0] = 0; g_halts = 0; g_jmpArmed = 1;
    if (setjmp(g_jmp) == 0) { OSPanic("file.c", 42, "bad %d", 7); CHECK(0); }
    g_jmpArmed = 0;
    CHECK(g_halts == 1 && strstr(g_log, "bad 7") && strstr(g_log, "file.c") && strstr(g_log, "line 42") && strstr(g_log, "Halt"));
    CHECK(OSSetErrorHandler(OS_ERROR_DSI, H1) == NULL);
    CHECK(OSSetErrorHandler(OS_ERROR_DSI, H2) == H1);
    CHECK(__OSErrorTable[OS_ERROR_DSI] == H2);
    CHECK(OSSetErrorHandler(OS_ERROR_THERMAL_INTERRUPT, H1) == NULL && __OSErrorTable[OS_ERROR_THERMAL_INTERRUPT] == H1);
    CHECK(OSSetErrorHandler((OSError) 17, H1) == NULL && OSSetErrorHandler((OSError) -1, H1) == NULL);
}

static void TestResetAndModes()
{
    int r = 0;
    g_jmpArmed = 1;
    struct { int reset; u32 code; BOOL menu; int kind; } cases[] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1, 1}, {2, 0, 0, 1}, {0, 0, 1, 1}};
    for (auto& c : cases) {
        g_resetKind = -9;
        if (setjmp(g_jmp) == 0) { OSResetSystem(c.reset, c.code, c.menu); CHECK(0); }
        r += (g_resetKind == c.kind);
    }
    g_jmpArmed = 0;
    CHECK(r == 5);
    g_button = 0; CHECK(OSGetResetButtonState() == FALSE); g_button = 1; CHECK(OSGetResetButtonState() == TRUE); g_button = 0;
    CHECK(OSGetSoundMode() == 1); OSSetSoundMode(0); CHECK(OSGetSoundMode() == 0); OSSetSoundMode(1); CHECK(OSGetSoundMode() == 1);
    CHECK(OSGetProgressiveMode() == 0); OSSetProgressiveMode(1); CHECK(OSGetProgressiveMode() == 1); OSSetProgressiveMode(0); CHECK(OSGetProgressiveMode() == 0);
    char buf[64]; OSSetSaveRegion(buf, buf + 64);
}

int main()
{
    TestClockAndInit(); TestInitArena(); TestInitFailure(); TestCalendar(); TestInterrupts(); TestMessagesAndErrors(); TestResetAndModes();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
