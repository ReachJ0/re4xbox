// Software GX command processor (the Flipper "GP"): consumes the byte stream the SDK writes to the write-gather
// FIFO (and display lists), keeps the CP / XF / BP register state, decodes vertices and hands primitives to a
// rendering backend. Plain C so the SDK's C files, the game and the host tests share it.
#ifndef XBOX_XGP_H
#define XBOX_XGP_H

#ifdef __cplusplus
extern "C" {
#endif

// primitive opcodes (command byte & 0xF8)
enum { XGP_QUADS = 0x80, XGP_TRIANGLES = 0x90, XGP_TRISTRIP = 0x98, XGP_TRIFAN = 0xA0, XGP_LINES = 0xA8,
       XGP_LINESTRIP = 0xB0, XGP_POINTS = 0xB8 };

// bits of XGpVertex.present
enum { XGP_HAS_POSMTX = 1u << 0, XGP_HAS_NRM = 1u << 1, XGP_HAS_COL0 = 1u << 2, XGP_HAS_COL1 = 1u << 3,
       XGP_HAS_TEX0 = 1u << 4 /* ..TEX7 = 1<<11 */, XGP_HAS_TEXMTX0 = 1u << 12 /* ..TEXMTX7 = 1<<19 */ };

typedef struct {
    float pos[3];
    float nrm[3];          // first normal (N, or N of NBT); the binormal/tangent are in nbt[]
    float nbt[6];
    unsigned char col[2][4];  // r g b a
    float tex[8][2];
    unsigned char posMtxIdx;
    unsigned char texMtxIdx[8];
    unsigned present;
} XGpVertex;

typedef struct {
    // CP
    unsigned vcdLo, vcdHi, vatA[8], vatB[8], vatC[8], arrBase[16], arrStride[16], matIdxA, matIdxB;
    // XF: 0x0000..0x0FFF matrix / light memory, 0x1000.. registers (indexed by address - 0x1000 below 0x100)
    unsigned xf[0x1000 + 0x100];
    // BP (24-bit values) and the pending write mask
    unsigned bp[256];
    unsigned bpMask;
    // statistics
    unsigned long long cmds, draws, verts;
} XGpState;

typedef struct {
    // a primitive batch; `v` holds `count` decoded vertices, in submission order
    void (*draw)(int prim, int vat, const XGpVertex* v, int count, const XGpState* st, void* user);
    // after every BP register write (reg, new 24-bit value); EFB copies, texture loads etc. are decoded by the backend
    void (*bp)(int reg, unsigned val, const XGpState* st, void* user);
    // finish (BP 0x45) and draw-sync token (BP 0x47/0x48) events
    void (*finish)(void* user);
    void (*token)(unsigned short token, void* user);
    void* user;
} XGpBackend;

// interrupt sink: the processor raises 0x12 (PE token) and 0x13 (PE finish) through it (gx_xbox.cpp -> SDK handlers)
void XGp_SetIrqSink(void (*sink)(int interrupt));

int XGx_DeliverInterrupts(void);                   // runs the queued PE token / finish handlers; returns how many
void XGx_Init(void);                               // XGp_Init + memory window + interrupt sink (called by GXInit)
void XGp_Init(void);                               // reset state and FIFO
void XGp_SetBackend(const XGpBackend* b);          // NULL = discard
const XGpState* XGp_State(void);
void XGp_Feed(const void* bytes, unsigned n);      // raw big-endian command bytes (a display list, a test stream)
void XGp_Flush(void);                              // process everything buffered
unsigned XGp_Pending(void);                        // bytes of an incomplete command still buffered

// memory the GP reads vertex arrays / display lists from: physical address (offset) -> pointer. Vertex data in
// memory is read BIG-endian (the game assets keep their GameCube byte order).
void XGp_SetMemory(unsigned char* base, unsigned size);
unsigned char* XGp_MemoryBase(unsigned* size);     // the window set above (null before XGx_Init); backends copy the EFB into it
unsigned XGp_VertexSize(int vat);                  // size in bytes of one direct vertex for the current VCD/VAT

// hardware register window (CP/PE/PI/MEM at physical 0x0C000000..0x0C009000)
void* XGx_HwPtr(unsigned paddr);

#ifdef __cplusplus
}
#endif
#endif
