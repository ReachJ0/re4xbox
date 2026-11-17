// Math layer of the Xbox port: the SDK's paired-single routines (PSMTX*, PSVEC*, PSQUAT*) and the GQR state of
// include/xbox/xps.h. The SDK ships a C reference for each routine (C_MTX*, ...); the PS_ names forward to it.
#ifdef RE4_XBOX

#include <dolphin/mtx.h>
#include <dolphin/base/PPCArch.h>

#include "xbox/xps.h"

uint32_t g_xpsGQR[8];

extern "C" {

void XPs_InitGQR(void)
{
    // what the game's task entry and main() program: li 3,n / oris 3,3,n / mtspr 914+n-4
    g_xpsGQR[2] = (4u << 16) | 4u;  // u8
    g_xpsGQR[3] = (5u << 16) | 5u;  // u16
    g_xpsGQR[4] = (6u << 16) | 6u;  // s8
    g_xpsGQR[5] = (7u << 16) | 7u;  // s16
}

#include "ps_wrappers.inc"

// PSMTXReorder: 3x4 matrix -> the "ROMtx" layout the skinning loops read (six pairs, see CalcSk1_x), one
// instruction per line of src/lib/psmtx.c.
void PSMTXReorder(const Mtx src, ROMtx dest)
{
    const uint8_t* s = (const uint8_t*) src;
    uint8_t* d = (uint8_t*) dest;
    XPs f0 = psq_l(s + 0, 0, 0);
    XPs f2 = psq_l(s + 16, 0, 0);
    XPs f4 = psq_l(s + 32, 0, 0);
    XPs f1 = psq_l(s + 8, 0, 0);
    XPs f6 = ps_merge00(f0, f2);
    XPs f3 = psq_l(s + 24, 0, 0);
    XPs f12 = ps_merge01(f4, f0);
    XPs f5 = psq_l(s + 40, 0, 0);
    XPs f7 = ps_merge11(f2, f4);
    psq_st(f6, d + 0, 0, 0);
    XPs f8 = ps_merge00(f1, f3);
    psq_st(f12, d + 8, 0, 0);
    XPs f9 = ps_merge01(f5, f1);
    psq_st(f7, d + 16, 0, 0);
    XPs f10 = ps_merge11(f3, f5);
    psq_st(f8, d + 24, 0, 0);
    psq_st(f9, d + 32, 0, 0);
    psq_st(f10, d + 40, 0, 0);
}

// Performance counters and the machine state register: the game only uses them for its profiling counters and the
// crash handler. They read as zero and writes are ignored.
void PPCMtpmc1(u32) {}
void PPCMtpmc2(u32) {}
void PPCMtpmc3(u32) {}
void PPCMtpmc4(u32) {}
void PPCMtmmcr0(u32) {}
void PPCMtmmcr1(u32) {}
u32 PPCMfpmc1(void) { return 0; }
void PPCMtmsr(u32) {}

}  // extern "C"
#endif
