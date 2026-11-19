// Crash report for the nxdk build. A title runs in ring 0 of the Xbox, and an exception nobody handles ends in
// the kernel's KeBugCheckEx, which freezes the machine (cli; hlt) without a word: the screen just stops.
// XCrash_Install() overwrites the first bytes of KeBugCheckEx in the kernel with a jump to Hook(), which writes
// the bugcheck code and parameters and the code addresses found on the stack to the text screen, then halts.
// For the usual KMODE_EXCEPTION_NOT_HANDLED (0x1E) the parameters are: exception code (0xC0000005 = access
// violation), address of the faulting instruction, 0 = read / 1 = write, address that was touched.
// The addresses go through tools/xbox/addr2sym.py with xbox/main.map to become function names.
// Not verified: that xemu and a real console let the kernel code be patched this way (CR0.WP is cleared for
// the write); if the patch itself failed, the symptom would be a crash at startup.
#ifdef NXDK
#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xbox/xtrace.h"

extern "C" {

static void __attribute__((stdcall, noreturn)) Hook(unsigned code, unsigned p1, unsigned p2, unsigned p3, unsigned p4)
{
    unsigned* sp = &code;   // the arguments of the call that was replaced; the return address is sp[-1]
    int shown = 0;

    XTrace_ShowTextScreen();
    debugClearScreen();   // the whole report has to fit on one screen: it would scroll the code off the top
    debugPrint("*** FATAL: bugcheck %08x ***\n", code);
    debugPrint("p1=%08x p2=%08x p3=%08x p4=%08x\n", p1, p2, p3, p4);
    if (code == 0x1E) {
        debugPrint("exception %08x at %08x, %s %08x\n", p1, p2, p3 ? "write" : "read", p4);
    }
    debugPrint("code addresses on the stack:\n");
    for (int i = -1; i < 1024 && shown < 32; i++) {
        unsigned v = sp[i];
        if (v >= 0x00011000 && v < 0x00400000) {   // inside the .text of the xbe
            debugPrint("%08x%c", v, (++shown % 8) ? ' ' : '\n');
        }
    }
    debugPrint("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void XCrash_Install(void)
{
    unsigned char* fn = (unsigned char*) (void*) KeBugCheckEx;   // dllimport: the address inside the kernel
    unsigned rel = (unsigned) (void*) &Hook - ((unsigned) fn + 5);
    unsigned cr0;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~0x10000u) : "memory");   // allow writing kernel code
    fn[0] = 0xE9;                                                           // jmp rel32
    *(unsigned*) (fn + 1) = rel;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

}
#endif
