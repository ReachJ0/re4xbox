// xbox/xos_backend_sys: XOsBackend_* for the console (nxdk) and, with the same file, for a PC.
//
// Assumptions about nxdk, none of them tried on hardware: KeQueryPerformanceCounter /
// KeQueryPerformanceFrequency (xboxkrnl) give a monotonic high resolution clock; time() reads the real-time
// clock; DbgPrint reaches the debug output; HalReturnToFirmware reboots. On a PC the POSIX equivalents are
// used so the layer can be exercised without a console.
#ifdef RE4_XBOX

#include "xbox/xos_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NXDK
#include <hal/debug.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {

int XOsBackend_Init(void) { return 1; }

unsigned long long XOsBackend_Nanoseconds(void)
{
#ifdef NXDK
    static unsigned long long freq = 0;
    unsigned long long now = (unsigned long long) KeQueryPerformanceCounter();
    if (!freq) {
        freq = (unsigned long long) KeQueryPerformanceFrequency();
    }
    // split to avoid overflowing 64 bits after a few hours
    return (now / freq) * 1000000000ull + (now % freq) * 1000000000ull / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000000ull + (unsigned long long) ts.tv_nsec;
#endif
}

unsigned int XOsBackend_WallSeconds2000(void)
{
    time_t now = time(NULL);
    const time_t epoch2000 = 946684800;  // 2000-01-01 00:00:00 UTC in Unix seconds
    return now > epoch2000 ? (unsigned int) (now - epoch2000) : 0;
}

void XOsBackend_Log(const char* text)
{
#ifdef NXDK
    DbgPrint("%s", text);
#else
    fputs(text, stderr);
#endif
}

void XOsBackend_Halt(void)
{
#ifdef NXDK
    HalReturnToFirmware(HalQuickRebootRoutine);
#else
    abort();
#endif
    for (;;) {}
}

void XOsBackend_Reset(int kind)
{
#ifdef NXDK
    HalReturnToFirmware(kind ? HalFatalErrorRebootRoutine : HalQuickRebootRoutine);
#else
    exit(kind ? 1 : 0);
#endif
    for (;;) {}
}

int XOsBackend_ResetButton(void) { return 0; }

void* XOsBackend_AllocMain(unsigned int size, unsigned int wanted)
{
#ifdef NXDK
    // Virtual memory at a fixed address; the pages are backed by RAM as they are committed.
    PVOID base = (PVOID) (unsigned long) wanted;
    SIZE_T len = size;
    if (NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        return base;
    }
    base = NULL;
    len = size;
    if (NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        return base;
    }
    return NULL;
#else
    void* p = mmap((void*) (unsigned long) wanted, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    }
    return p == MAP_FAILED ? NULL : p;
#endif
}

}  // extern "C"

#endif  // RE4_XBOX
