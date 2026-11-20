// Crash report v2: compact on-screen summary (no scroll), full report to E:\re4crash.log.
// All reads are range-checked: a fault inside the handler must never re-enter it.
// A title runs in ring 0; an unhandled exception ends in KeBugCheckEx (cli; hlt). XCrash_Install() patches its
// first bytes with a jump to Hook(). For bugcheck 0x1E the parameters are: exception code (0xC0000005 = access
// violation), faulting instruction, 0 = read / 1 = write, address touched. Addresses go through
// tools/xbox/addr2sym.py with xbox/main.map. Not verified on xemu/hardware: the kernel patch (CR0.WP cleared),
// and that E: is mountable/writable at crash time (the log is best effort; the screen report never depends on it).
#ifdef NXDK
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/mount.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "xbox/xtrace.h"

extern "C" {

static FILE* g_log;
static int g_depth;
static unsigned g_stkLo, g_stkHi;   // the crashing thread's stack (kernel: StackLimit .. StackBase), 0 if unknown

// Readable RAM windows (64 MB mirrors). Anything else may fault: MMIO, unmapped, PCI.
static int SafePtr(const void* p, unsigned n)
{
    unsigned a = (unsigned) p, top = a + n;
    if (top < a) return 0;
    if (a >= 0x00010000u && top <= 0x04000000u) return 1;  // low mirror: xbe, heap, stack
    // not the 0x80000000 / 0xC0000000 mirrors: xemu showed pages there that are not mapped (a read faulted)
    if (g_stkHi && a >= g_stkLo && top <= g_stkHi) return 1;  // this thread's stack (0xD0000000 range)
    return 0;
}

static void Log(const char* fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
}

static void Both(const char* fmt, ...)   // screen + file
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    debugPrint("%s", b);
    Log("%s", b);
}

static void __attribute__((stdcall, noreturn))
Hook(unsigned code, unsigned p1, unsigned p2, unsigned p3, unsigned p4)
{
    unsigned* sp = &code;   // ~ESP at hook entry; return address at sp[-1]
    unsigned eip = 0, fault = 0;
    const char* kind = "read";
    unsigned hits[32];
    int nh = 0, i, j;

    if (g_depth++) {   // fault inside the handler: say so and die quietly
        debugPrint("DOUBLE FAULT in crash handler\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    // the thread stack lives in 0xD0000000..0xD4000000: the 4 KB above esp are inside it (we are in it), so readable
    if ((unsigned) sp >= 0xD0000000u && (unsigned) sp < 0xD4000000u - 0x1000u) {
        g_stkLo = (unsigned) sp;
        g_stkHi = (unsigned) sp + 0x1000u;
    }
    XTrace_ShowTextScreen();   // pbkit may own the screen; switch to the text one
    debugClearScreen();        // the report must fit on one screen
    nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");   // no-op/fails if already mounted: ignored
    g_log = fopen("E:\\re4crash.log", "a");
    Log("\n==== crash ====\n");
    Log("params %08x %08x %08x %08x\n", p1, p2, p3, p4);

    if (code == 0x1E) { eip = p2; fault = p4; kind = p3 ? "write" : "read"; }

    Both("*** RE4 CRASH *** bugcheck %08x\n", code);
    Both("exception %08x at eip %08x\n", p1, eip);
    Both("%s of %08x, esp %08x\n", kind, fault, (unsigned) sp);
    Both("stack %08x..%08x\n", g_stkLo, g_stkHi);

    // 16 code bytes at EIP (only if inside readable RAM)
    if (eip && SafePtr((const void*) (eip - 8), 24)) {
        const unsigned char* c = (const unsigned char*) (eip - 8);
        Both("code:");
        for (i = 0; i < 16; i++) Both(" %02x", c[i]);
        Both("\n");
    }

    // candidate return addresses on the stack, without dereferencing them
    for (i = 0; i < 1024 && nh < 32; i++) {
        unsigned* q = sp + i;
        unsigned v;
        if (!SafePtr(q, 4)) break;
        v = *q;
        if (v >= 0x00011000u && v < 0x04000000u) hits[nh++] = v;   // inside .text
    }
    Both("stack (%d):\n", nh);
    for (i = 0; i < nh; i += 4) {
        Both(" ");
        for (j = i; j < nh && j < i + 4; j++) Both(" %08x", hits[j]);
        Both("\n");
    }

    // memory around the fault address: file only, and only if it is RAM
    if (fault && SafePtr((const void*) (fault & ~15u), 32)) {
        const unsigned char* p = (const unsigned char*) (fault & ~15u);
        Log("mem @ %08x:", fault);
        for (i = 0; i < 32; i++) {
            if ((i & 15) == 0) Log("\n ");
            Log(" %02x", p[i]);
        }
        Log("\n");
    } else if (fault) {
        Both("fault addr outside RAM (not dumped)\n");
    }

    Both("last trace:\n");   // what the game said before it died
    for (i = XTRACE_NLINES - 5; i < XTRACE_NLINES; i++) {
        const char* l = XTrace_Line(i);
        if (l[0]) Both(" %s\n", l);
    }
    Both("full log: E:\\re4crash.log\n");
    Both("map: python3 tools/xbox/addr2sym.py xbox/main.map\n");
    if (g_log) { fclose(g_log); g_log = 0; }
    for (;;) __asm__ volatile("cli; hlt");
}

void XCrash_Install(void)
{
    unsigned char* fn = (unsigned char*) (void*) KeBugCheckEx;
    unsigned rel = (unsigned) (void*) &Hook - ((unsigned) fn + 5);
    unsigned cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~0x10000u) : "memory");
    fn[0] = 0xE9;
    *(unsigned*) (fn + 1) = rel;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

}  // extern "C"
#endif  // NXDK
