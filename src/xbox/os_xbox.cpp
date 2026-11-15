// xbox/os_xbox: Xbox replacement for the parts of the Dolphin SDK OS library that need the platform
// (src/lib/OS.c, OSTime.c, OSInterrupt.c, OSError.c, OSFatal.c, OSReset.c, OSResetSW.c, OSRtc.c, OSMemory.c).
//
// Provides:
//   OSInit OSGetConsoleType OSGetConsoleSimulatedMemSize
//   OSGetTime OSGetTick OSTicksToCalendarTime OSCalendarTimeToTicks
//   OSDisableInterrupts OSEnableInterrupts OSRestoreInterrupts
//   OSReport OSPanic OSSetErrorHandler (+ __OSErrorTable)
//   OSResetSystem OSGetResetButtonState OSSetSaveRegion
//   OSGetSoundMode OSSetSoundMode OSGetProgressiveMode OSSetProgressiveMode
//   and the globals __OSBusClock / __OSCoreClock that the OS_*_CLOCK macros read.
// Link it together with the SDK files that are plain C and need nothing from the platform, unchanged:
//   src/lib/OSAlloc.c (heaps), src/lib/OSArena.c (arena), src/lib/OSStopwatch.c.
// NOT here yet (a later step): threads, semaphores, alarms, OSLink / OSUnlink / OSGetSectionInfo, the
// ROM font (OSInitFont, OSGetFontTexture, OSGetFontEncode). The game uses them in scheduler.cpp,
// main_sub.cpp, exception.cpp and dvd.cpp.
//
// Time: OSGetTime is ticks of the GameCube time base (bus clock / 4 = 40.5 MHz) since 2000-01-01, like the
// RTC-backed counter the game's save timestamps come from: the wall clock at start plus the monotonic time
// since. OSGetTick is the same counter counted from start, low 32 bits.
//
// Interrupts: there are none to mask. OSDisableInterrupts / OSRestoreInterrupts keep the flag with the
// SDK's return values so the game's save-and-restore code behaves, and nothing else. Disc and ARAM
// completions are delivered by XDvd_Pump (xbox/xdvd.h), which does not look at this flag.
//
// Error handlers (exception.cpp registers its crash screen) are stored and returned, but no hardware
// exception reaches them: the Xbox has its own fault handling.
//
// Only compiled for the Xbox build (RE4_XBOX); it has no meaning on the GameCube target.
#ifdef RE4_XBOX

#include <dolphin/os.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xbox/xos_backend.h"

#define BUS_CLOCK 162000000u   // the GameCube's bus clock; the time base runs at a quarter of it
#define CORE_CLOCK 486000000u
#define SIM_MEM_SIZE 0x01800000u    // the 24 MB the game was written for
#define ARENA_SIZE 0x01200000u      // part of it handed to the game's heaps (see OSInit)
#define CONSOLE_TYPE_RETAIL 0x00000001u
#define MAX_LINE 512

extern "C" {

u32 __OSBusClock = BUS_CLOCK;
u32 __OSCoreClock = CORE_CLOCK;
OSErrorHandler __OSErrorTable[17];

}

namespace {

int s_inited = 0;
int s_arena = 0;
int s_interrupts = 1;
unsigned long long s_ns0 = 0;
OSTime s_wallBase = 0;  // wall clock at start, in ticks since 2000
u32 s_soundMode = 1;        // stereo
u32 s_progressive = 0;
void* s_saveStart = 0;
void* s_saveEnd = 0;

// ticks of the time base since start
OSTime MonoTicks()
{
    unsigned long long ns = XOsBackend_Nanoseconds() - s_ns0;
    unsigned long long hz = BUS_CLOCK / 4;

    return (OSTime) ((ns / 1000000000ull) * hz + (ns % 1000000000ull) * hz / 1000000000ull);
}

void EnsureInit()
{
    if (s_inited) {
        return;
    }
    s_inited = 1;
    XOsBackend_Init();
    s_ns0 = XOsBackend_Nanoseconds();
    s_wallBase = (OSTime) XOsBackend_WallSeconds2000() * (BUS_CLOCK / 4);
}

// calendar tables and helpers, as in src/lib/OSTime.c
const int YearDays[MONTH_MAX] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
const int LeapYearDays[MONTH_MAX] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

int IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int GetYearDays(int year, int mon) { return (IsLeapYear(year) ? LeapYearDays : YearDays)[mon]; }

int GetLeapDays(int year)
{
    if (year < 1) {
        return 0;
    }
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

void GetDates(int days, OSCalendarTime* td)
{
    int year;
    int n;
    int month;
    const int* md;

    td->wday = (days + 6) % WEEK_DAY_MAX;
    for (year = days / YEAR_DAY_MAX; days < (n = year * YEAR_DAY_MAX + GetLeapDays(year)); year--) {
    }
    days -= n;
    td->year = year;
    td->yday = days;
    md = IsLeapYear(year) ? LeapYearDays : YearDays;
    for (month = MONTH_MAX; days < md[--month];) {
    }
    td->mon = month;
    td->mday = days - md[month] + 1;
}

}  // namespace

extern "C" {

// ---- start-up and facts about the machine -------------------------------------------------------------

// Once at boot. Gives the game its arena: ARENA_SIZE bytes of the main memory block (the real arena is what
// the 24 MB leave after the program image and the file system table; the exact figure is a placeholder
// until the memory map is decided).
void OSInit(void)
{
    void* block;

    if (s_arena) {
        return;
    }
    EnsureInit();
    block = XOsBackend_AllocMain(ARENA_SIZE);
    if (!block) {
        OSPanic(__FILE__, __LINE__, "OSInit(): no memory for the arena");
    }
    s_arena = 1;
    OSSetArenaLo(block);
    OSSetArenaHi((u8*) block + ARENA_SIZE);
}

u32 OSGetConsoleType(void) { return CONSOLE_TYPE_RETAIL; }

u32 OSGetConsoleSimulatedMemSize(void) { return SIM_MEM_SIZE; }

void OSRegisterVersion(const char*) {}

// ---- time ---------------------------------------------------------------------------------------------

OSTime OSGetTime(void)
{
    EnsureInit();
    return s_wallBase + MonoTicks();
}

OSTick OSGetTick(void)
{
    EnsureInit();
    return (OSTick) MonoTicks();
}

// Same arithmetic as src/lib/OSTime.c (Gregorian dates counted from year 0, the SDK's day bias).
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
    int days;
    int secs;
    OSTime d;

    d = ticks % OS_SEC_TO_TICKS(1);
    if (d < 0) {
        d += OS_SEC_TO_TICKS(1);
    }
    td->usec = OS_TICKS_TO_USEC(d) % USEC_MAX;
    td->msec = OS_TICKS_TO_MSEC(d) % MSEC_MAX;
    ticks -= d;

    days = (OS_TICKS_TO_SEC(ticks) / SECS_IN_DAY) + BIAS;
    secs = OS_TICKS_TO_SEC(ticks) % SECS_IN_DAY;
    if (secs < 0) {
        days -= 1;
        secs += SECS_IN_DAY;
    }
    GetDates(days, td);
    td->hour = secs / 60 / 60;
    td->min = secs / 60 % 60;
    td->sec = secs % 60;
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td)
{
    OSTime secs;
    int ov_mon = td->mon / MONTH_MAX;
    int mon = td->mon - (ov_mon * MONTH_MAX);
    int year;

    if (mon < 0) {
        mon += MONTH_MAX;
        ov_mon--;
    }
    year = td->year + ov_mon;
    secs = (OSTime) SECS_IN_YEAR * year +
           (OSTime) SECS_IN_DAY * (GetLeapDays(year) + GetYearDays(year, mon) + td->mday - 1) +
           (OSTime) SECS_IN_HOUR * td->hour + (OSTime) SECS_IN_MIN * td->min + td->sec - (OSTime) 0xEB1E1BF80ULL;
    return OS_SEC_TO_TICKS(secs) + OS_MSEC_TO_TICKS((OSTime) td->msec) + OS_USEC_TO_TICKS((OSTime) td->usec);
}

// ---- interrupts (a flag only) ---------------------------------------------------------------------------

BOOL OSDisableInterrupts(void)
{
    BOOL was = s_interrupts;
    s_interrupts = 0;
    return was;
}

BOOL OSEnableInterrupts(void)
{
    BOOL was = s_interrupts;
    s_interrupts = 1;
    return was;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    BOOL was = s_interrupts;
    s_interrupts = level ? 1 : 0;
    return was;
}

// ---- messages and errors ----------------------------------------------------------------------------------

void OSReport(const char* msg, ...)
{
    char line[MAX_LINE];
    va_list ap;

    va_start(ap, msg);
    vsnprintf(line, sizeof(line), msg, ap);
    va_end(ap);
    XOsBackend_Log(line);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    char text[MAX_LINE];
    va_list ap;

    va_start(ap, msg);
    vsnprintf(text, sizeof(text), msg, ap);
    va_end(ap);
    OSReport("%s", text);
    OSReport(" in \"%s\" on line %d.\n", file, line);
    OSReport("\nHalt\n");
    XOsBackend_Halt();
    for (;;) {}
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    OSErrorHandler old;

    if ((u32) error >= sizeof(__OSErrorTable) / sizeof(__OSErrorTable[0])) {
        return 0;
    }
    old = __OSErrorTable[error];
    __OSErrorTable[error] = handler;
    return old;
}

// ---- reset and settings -----------------------------------------------------------------------------------

void OSResetSystem(int reset, u32, BOOL forceMenu)
{
    // reset: OS_RESET_RESTART (0) / HOTRESET (1) start the game again, SHUTDOWN (2) turns the console off;
    // forceMenu is "go to the IPL menu instead". The Xbox equivalents: start again, or the dashboard.
    XOsBackend_Reset((reset == OS_RESET_SHUTDOWN || forceMenu) ? 1 : 0);
    for (;;) {}
}

BOOL OSGetResetButtonState(void) { return XOsBackend_ResetButton() ? TRUE : FALSE; }

void OSSetSaveRegion(void* start, void* end)
{
    s_saveStart = start;
    s_saveEnd = end;
}

u32 OSGetSoundMode(void) { return s_soundMode; }

void OSSetSoundMode(u32 mode) { s_soundMode = mode ? 1 : 0; }

u32 OSGetProgressiveMode(void) { return s_progressive; }

void OSSetProgressiveMode(u32 on) { s_progressive = on ? 1 : 0; }

}  // extern "C"

#endif  // RE4_XBOX
