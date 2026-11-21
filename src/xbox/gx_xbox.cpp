// OS-side glue the SDK's GX sources expect from the console: the interrupt table (the command processor raises
// the PE token / finish interrupts through it), the write-gather and bus registers and the reset hook list. The
// rest of GX is the SDK itself (src/lib/GX*.c) running on the software command processor (gp_xbox.cpp).
#ifdef RE4_XBOX
#include <dolphin/os.h>
#include <xbox/xgp.h>
#include <xbox/xmem.h>

extern "C" {

// The host tests of the GX layer do not link the thread layer; the Xbox build overrides this with the real one.
__attribute__((weak)) OSContext* OSGetCurrentContext(void) { return 0; }

#define XGX_NUM_INTR 32
static __OSInterruptHandler s_handlers[XGX_NUM_INTR];
static OSInterruptMask s_mask;

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler)
{
    __OSInterruptHandler old;
    if ((unsigned) interrupt >= XGX_NUM_INTR) {
        return 0;
    }
    old = s_handlers[interrupt];
    s_handlers[interrupt] = handler;
    return old;
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = s_mask;
    s_mask |= mask;
    return old;
}

OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = s_mask;
    s_mask &= ~mask;
    return old;
}

// The processor raises an interrupt while the SDK still holds interrupts disabled (GXSetDrawDone writes the finish
// command, then clears DrawDone before restoring them); on the console the handler runs later. The events are
// therefore queued and delivered at the points where the CPU would take them: after the SDK restores interrupts,
// when it would sleep waiting for one, and once per frame.
static unsigned s_pending;

static void RaiseInterrupt(int interrupt)
{
    if ((unsigned) interrupt < XGX_NUM_INTR) {
        s_pending |= 1u << interrupt;
    }
}

// returns the number of handlers run
int XGx_DeliverInterrupts(void)
{
    int n = 0, i;
    for (i = 0; i < XGX_NUM_INTR && s_pending; i++) {
        if ((s_pending & (1u << i)) == 0) {
            continue;
        }
        s_pending &= ~(1u << i);
        if (s_handlers[i]) {
            // The SDK handlers end with OSSetCurrentContext(context): hand them the context that was current, or the
            // thread scheduler would be left with no current context and never switch threads again.
            s_handlers[i]((__OSInterrupt) i, OSGetCurrentContext());
            n++;
        }
    }
    return n;
}

void XGx_Init(void)
{
    XGp_Init();
    s_pending = 0;
    XGp_SetMemory((unsigned char*) (unsigned long) g_xmemBase, XMEM_SPAN);
    XGp_SetIrqSink(RaiseInterrupt);
}

// write-gather pipe address register: nothing to program, stores go straight to the processor
u32 PPCMfwpar(void) { return 0; }
void PPCMtwpar(u32) {}

// reset hooks: the console never resets itself
void OSRegisterResetFunction(OSResetFunctionInfo*) {}

}
#endif
