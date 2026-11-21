// Software GX command processor, see include/xbox/xgp.h. Command and vertex formats follow the Flipper GP:
//   00 NOP | 08 reg32 CP load | 10 hdr32 data.. XF load | 20/28/30/38 idx32 indexed XF load | 40 addr32 size32 call DL
//   48 invalidate vertex cache | 61 bp32 BP load | 80..B8|vat count16 vertices
#include <stdlib.h>
#include <string.h>
#include <xbox/xgp.h>

#define XF_MEM 0x1000u
#define XF_REGS 0x100u

static XGpState g_st;
static XGpBackend g_be;
static int g_haveBe;
static unsigned char* g_mem;
static unsigned g_memSize;
static unsigned char* g_buf;
static unsigned g_len, g_cap, g_pos;
static XGpVertex* g_vtx;
static unsigned g_vtxCap;
static int g_dlDepth;
static void (*g_irq)(int);
static unsigned g_need;  // size of the draw command being received (0 = unknown), to skip re-parsing it per store
static unsigned char g_hw[0x9000 + 64];

// ---------------------------------------------------------------- big-endian readers
static unsigned be16(const unsigned char* p) { return ((unsigned) p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char* p) { return ((unsigned) p[0] << 24) | ((unsigned) p[1] << 16) | ((unsigned) p[2] << 8) | p[3]; }
static float bef(const unsigned char* p)
{
    unsigned u = be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

extern "C" void* XGx_HwPtr(unsigned paddr)
{
    unsigned off = paddr - 0x0C000000u;
    return off < 0x9000u ? (void*) (g_hw + off) : (void*) (g_hw + 0x9000);
}

extern "C" void XGp_SetBackend(const XGpBackend* b)
{
    if (b) {
        g_be = *b;
        g_haveBe = 1;
    } else {
        memset(&g_be, 0, sizeof g_be);
        g_haveBe = 0;
    }
}
extern "C" void XGp_SetIrqSink(void (*sink)(int)) { g_irq = sink; }
extern "C" const XGpState* XGp_State(void) { return &g_st; }
extern "C" void XGp_SetMemory(unsigned char* base, unsigned size) { g_mem = base; g_memSize = size; }
extern "C" unsigned char* XGp_MemoryBase(unsigned* size)
{
    if (size) *size = g_memSize;
    return g_mem;
}

extern "C" void XGp_Init(void)
{
    memset(&g_st, 0, sizeof g_st);
    g_st.bpMask = 0xFFFFFF;
    g_len = g_pos = 0;
    g_need = 0;
    g_dlDepth = 0;
    memset(g_hw, 0, sizeof g_hw);
    ((unsigned short*) (g_hw + 0x0000))[0] = 0x000C;  // CP status: read idle, command idle
}

// ---------------------------------------------------------------- vertex layout
// attribute order inside a vertex: PosMtxIdx, Tex0..7 MtxIdx, Pos, Nrm, Col0, Col1, Tex0..7
static const int kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};

static int PosComps(unsigned a) { return 2 + (a & 1); }
static int NrmFmt(unsigned a) { return (a >> 10) & 7; }

// bytes of the inline data of one attribute given its VCD type (0 none, 1 direct, 2 index8, 3 index16)
static int Sz(int type, int direct) { return type == 0 ? 0 : type == 1 ? direct : type == 2 ? 1 : 2; }

static int ColSize(int fmt)
{
    static const int s[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return s[fmt & 7];
}

struct Layout {
    int type[2 + 8 + 8];  // [0]=pos [1]=nrm ; col via direct fields below
    int posT, nrmT, colT[2], texT[8], posMtx, texMtx[8];
    int posN, posF, posShift, nrmN, nrmF, nrmIdx3, colF[2], colN[2], texN[8], texF[8], texShift[8];
    int size;
};

static void MakeLayout(int vat, Layout* L)
{
    unsigned lo = g_st.vcdLo, hi = g_st.vcdHi, A = g_st.vatA[vat], B = g_st.vatB[vat], C = g_st.vatC[vat];
    int i, sz = 0;
    L->posMtx = lo & 1;
    for (i = 0; i < 8; i++) L->texMtx[i] = (lo >> (1 + i)) & 1;
    L->posT = (lo >> 9) & 3;
    L->nrmT = (lo >> 11) & 3;
    L->colT[0] = (lo >> 13) & 3;
    L->colT[1] = (lo >> 15) & 3;
    for (i = 0; i < 8; i++) L->texT[i] = (hi >> (2 * i)) & 3;
    L->posN = PosComps(A);
    L->posF = (A >> 1) & 7;
    L->posShift = (A >> 4) & 31;
    L->nrmN = (A >> 9) & 1 ? 9 : 3;
    L->nrmF = NrmFmt(A);
    L->nrmIdx3 = (A >> 31) & 1;
    L->colN[0] = ((A >> 13) & 1) ? 4 : 3;
    L->colF[0] = (A >> 14) & 7;
    L->colN[1] = ((A >> 17) & 1) ? 4 : 3;
    L->colF[1] = (A >> 18) & 7;
    // texcoord fields: (cnt bit, fmt shift, shift pos) per tex unit
    L->texN[0] = 1 + ((A >> 21) & 1); L->texF[0] = (A >> 22) & 7; L->texShift[0] = (A >> 25) & 31;
    L->texN[1] = 1 + (B & 1);         L->texF[1] = (B >> 1) & 7;   L->texShift[1] = (B >> 4) & 31;
    L->texN[2] = 1 + ((B >> 9) & 1);  L->texF[2] = (B >> 10) & 7;  L->texShift[2] = (B >> 13) & 31;
    L->texN[3] = 1 + ((B >> 18) & 1); L->texF[3] = (B >> 19) & 7;  L->texShift[3] = (B >> 22) & 31;
    L->texN[4] = 1 + ((B >> 27) & 1); L->texF[4] = (B >> 28) & 7;  L->texShift[4] = C & 31;
    L->texN[5] = 1 + ((C >> 5) & 1);  L->texF[5] = (C >> 6) & 7;   L->texShift[5] = (C >> 9) & 31;
    L->texN[6] = 1 + ((C >> 14) & 1); L->texF[6] = (C >> 15) & 7;  L->texShift[6] = (C >> 18) & 31;
    L->texN[7] = 1 + ((C >> 23) & 1); L->texF[7] = (C >> 24) & 7;  L->texShift[7] = (C >> 27) & 31;

    sz += L->posMtx;
    for (i = 0; i < 8; i++) sz += L->texMtx[i];
    sz += Sz(L->posT, L->posN * kCompSize[L->posF & 7]);
    {
        int nrmDirect = L->nrmN * kCompSize[L->nrmF & 7];
        if (L->nrmT == 1) sz += nrmDirect;
        else if (L->nrmT) sz += (L->nrmIdx3 && L->nrmN == 9 ? 3 : 1) * Sz(L->nrmT, 0);
    }
    for (i = 0; i < 2; i++) sz += Sz(L->colT[i], ColSize(L->colF[i]));
    for (i = 0; i < 8; i++) sz += Sz(L->texT[i], L->texN[i] * kCompSize[L->texF[i] & 7]);
    L->size = sz;
}

extern "C" unsigned XGp_VertexSize(int vat)
{
    Layout L;
    MakeLayout(vat & 7, &L);
    return (unsigned) L.size;
}

// ---------------------------------------------------------------- attribute decoding
static float Comp(const unsigned char* p, int fmt, int shift)
{
    switch (fmt) {
    case 0: return (float) p[0] / (float) (1u << shift);
    case 1: return (float) (signed char) p[0] / (float) (1u << shift);
    case 2: return (float) be16(p) / (float) (1u << shift);
    case 3: return (float) (short) be16(p) / (float) (1u << shift);
    default: return bef(p);
    }
}

static void Color(const unsigned char* p, int fmt, unsigned char* out)
{
    unsigned v;
    out[3] = 255;
    switch (fmt) {
    case 0:  // RGB565
        v = be16(p);
        out[0] = (unsigned char) ((((v >> 11) & 31) << 3) | (((v >> 11) & 31) >> 2));
        out[1] = (unsigned char) ((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4));
        out[2] = (unsigned char) (((v & 31) << 3) | ((v & 31) >> 2));
        break;
    case 1:
    case 2:  // RGB8 / RGBX8
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        break;
    case 3:  // RGBA4
        v = be16(p);
        out[0] = (unsigned char) (((v >> 12) & 15) * 17);
        out[1] = (unsigned char) (((v >> 8) & 15) * 17);
        out[2] = (unsigned char) (((v >> 4) & 15) * 17);
        out[3] = (unsigned char) ((v & 15) * 17);
        break;
    case 4:  // RGBA6
        v = ((unsigned) p[0] << 16) | ((unsigned) p[1] << 8) | p[2];
        out[0] = (unsigned char) ((((v >> 18) & 63) << 2) | (((v >> 18) & 63) >> 4));
        out[1] = (unsigned char) ((((v >> 12) & 63) << 2) | (((v >> 12) & 63) >> 4));
        out[2] = (unsigned char) ((((v >> 6) & 63) << 2) | (((v >> 6) & 63) >> 4));
        out[3] = (unsigned char) (((v & 63) << 2) | ((v & 63) >> 4));
        break;
    default:  // RGBA8
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        break;
    }
}

static const unsigned char* ArrayPtr(int arr, unsigned idx, int need)
{
    unsigned long long off = (unsigned long long) g_st.arrBase[arr] + (unsigned long long) idx * g_st.arrStride[arr];
    if (!g_mem || off + (unsigned) need > g_memSize) return 0;
    return g_mem + off;
}

static unsigned ReadIdx(const unsigned char*& p, int type)
{
    unsigned v;
    if (type == 2) { v = p[0]; p += 1; }
    else { v = be16(p); p += 2; }
    return v;
}

// Decodes one vertex at p (advances p). Returns 0 when an indexed attribute points outside memory.
static int DecodeVertex(const Layout& L, const unsigned char*& p, XGpVertex* v)
{
    int i, c, ok = 1;
    const unsigned char* d;
    memset(v, 0, sizeof *v);
    if (L.posMtx) { v->posMtxIdx = *p++; v->present |= XGP_HAS_POSMTX; }
    for (i = 0; i < 8; i++)
        if (L.texMtx[i]) { v->texMtxIdx[i] = *p++; v->present |= XGP_HAS_TEXMTX0 << i; }

    if (L.posT) {
        int bytes = L.posN * kCompSize[L.posF & 7];
        d = L.posT == 1 ? p : ArrayPtr(0, ReadIdx(p, L.posT), bytes);
        if (L.posT == 1) p += bytes;
        if (d) for (c = 0; c < L.posN; c++) v->pos[c] = Comp(d + c * kCompSize[L.posF & 7], L.posF, L.posF == 4 ? 0 : L.posShift);
        else ok = 0;
    }
    if (L.nrmT) {
        int cs = kCompSize[L.nrmF & 7], shift = L.nrmF == 1 ? 6 : L.nrmF == 3 ? 14 : L.nrmF == 0 ? 7 : 15;
        int n = L.nrmN;
        v->present |= XGP_HAS_NRM;
        if (L.nrmT == 1 || !(L.nrmIdx3 && n == 9)) {
            d = L.nrmT == 1 ? p : ArrayPtr(1, ReadIdx(p, L.nrmT), n * cs);
            if (L.nrmT == 1) p += n * cs;
            if (d) {
                for (c = 0; c < n; c++) {
                    float f = Comp(d + c * cs, L.nrmF, L.nrmF == 4 ? 0 : shift);
                    if (c < 3) v->nrm[c] = f; else v->nbt[c - 3] = f;
                }
            } else ok = 0;
        } else {  // NBT with three separate indices
            for (i = 0; i < 3; i++) {
                d = ArrayPtr(1, ReadIdx(p, L.nrmT), 3 * cs);
                if (!d) { ok = 0; continue; }
                for (c = 0; c < 3; c++) {
                    float f = Comp(d + c * cs, L.nrmF, L.nrmF == 4 ? 0 : shift);
                    if (i == 0) v->nrm[c] = f; else v->nbt[(i - 1) * 3 + c] = f;
                }
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (!L.colT[i]) continue;
        v->present |= XGP_HAS_COL0 << i;
        {
            int bytes = ColSize(L.colF[i]);
            d = L.colT[i] == 1 ? p : ArrayPtr(2 + i, ReadIdx(p, L.colT[i]), bytes);
            if (L.colT[i] == 1) p += bytes;
            if (d) Color(d, L.colF[i], v->col[i]); else ok = 0;
        }
    }
    for (i = 0; i < 8; i++) {
        int cs, bytes;
        if (!L.texT[i]) continue;
        cs = kCompSize[L.texF[i] & 7];
        bytes = L.texN[i] * cs;
        v->present |= XGP_HAS_TEX0 << i;
        d = L.texT[i] == 1 ? p : ArrayPtr(4 + i, ReadIdx(p, L.texT[i]), bytes);
        if (L.texT[i] == 1) p += bytes;
        if (d) for (c = 0; c < L.texN[i]; c++) v->tex[i][c] = Comp(d + c * cs, L.texF[i], L.texF[i] == 4 ? 0 : L.texShift[i]);
        else ok = 0;
    }
    return ok;
}

// ---------------------------------------------------------------- register files
static void BpWrite(unsigned word)
{
    unsigned reg = word >> 24, val = word & 0xFFFFFF;
    if (reg == 0xFE) {  // write mask for the next BP write
        g_st.bpMask = val;
        return;
    }
    g_st.bp[reg] = (g_st.bp[reg] & ~g_st.bpMask) | (val & g_st.bpMask);
    val = g_st.bp[reg];
    g_st.bpMask = 0xFFFFFF;
    if (reg == 0x47 || reg == 0x48) {  // draw-sync token (0x48 also raises the interrupt)
        unsigned short* pe = (unsigned short*) (g_hw + 0x1000);
        pe[7] = (unsigned short) (val & 0xFFFF);
        if (reg == 0x48) {
            if (g_haveBe && g_be.token) g_be.token((unsigned short) (val & 0xFFFF), g_be.user);
            if (g_irq) g_irq(0x12);
        }
    }
    if (reg == 0x45) {  // PE finish
        if (g_haveBe && g_be.finish) g_be.finish(g_be.user);
        if (g_irq) g_irq(0x13);
    }
    if (g_haveBe && g_be.bp) g_be.bp((int) reg, val, &g_st, g_be.user);
}

static void CpWrite(unsigned reg, unsigned val)
{
    if (reg == 0x30) g_st.matIdxA = val;
    else if (reg == 0x40) g_st.matIdxB = val;
    else if (reg == 0x50) g_st.vcdLo = val;
    else if (reg == 0x60) g_st.vcdHi = val;
    else if (reg >= 0x70 && reg < 0x78) g_st.vatA[reg - 0x70] = val;
    else if (reg >= 0x80 && reg < 0x88) g_st.vatB[reg - 0x80] = val;
    else if (reg >= 0x90 && reg < 0x98) g_st.vatC[reg - 0x90] = val;
    else if (reg >= 0xA0 && reg < 0xB0) g_st.arrBase[reg - 0xA0] = val & 0x03FFFFFF;
    else if (reg >= 0xB0 && reg < 0xC0) g_st.arrStride[reg - 0xB0] = val & 0xFF;
}

static void XfWrite(unsigned addr, unsigned val)
{
    if (addr < XF_MEM) g_st.xf[addr] = val;
    else if (addr < XF_MEM + XF_REGS) g_st.xf[addr] = val;
}

// ---------------------------------------------------------------- command parser
static unsigned ParseAll(const unsigned char* p, unsigned n);

static void Draw(int op, int vat, const unsigned char* p, unsigned count)
{
    Layout L;
    unsigned i;
    int ok = 1;
    if (count > g_vtxCap) {
        g_vtxCap = count;
        g_vtx = (XGpVertex*) realloc(g_vtx, g_vtxCap * sizeof(XGpVertex));
    }
    MakeLayout(vat, &L);
    for (i = 0; i < count; i++) ok &= DecodeVertex(L, p, &g_vtx[i]);
    g_st.draws++;
    g_st.verts += count;
    (void) ok;
    if (g_haveBe && g_be.draw) g_be.draw(op & 0xF8, vat, g_vtx, (int) count, &g_st, g_be.user);
}

// Parses one command at p. Returns its size, 0 when the data is incomplete, or (unsigned) -1 for an unknown byte.
static unsigned ParseOne(const unsigned char* p, unsigned n, unsigned* needed)
{
    unsigned op = p[0], sz;
    *needed = 0;
    if (op == 0x00 || op == 0x48) return 1;
    if (op == 0x08) {
        if (n < 6) return 0;
        CpWrite(p[1], be32(p + 2));
        return 6;
    }
    if (op == 0x10) {
        unsigned hdr, cnt, addr, i;
        if (n < 5) return 0;
        hdr = be32(p + 1);
        cnt = ((hdr >> 16) & 15) + 1;
        addr = hdr & 0xFFFF;
        sz = 5 + 4 * cnt;
        if (n < sz) return 0;
        for (i = 0; i < cnt; i++) XfWrite(addr + i, be32(p + 5 + 4 * i));
        return sz;
    }
    if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        unsigned w, idx, cnt, addr, i, arr = 12 + ((op - 0x20) >> 3);
        const unsigned char* d;
        if (n < 5) return 0;
        w = be32(p + 1);
        idx = w >> 16;
        cnt = ((w >> 12) & 15) + 1;
        addr = w & 0xFFF;
        d = ArrayPtr((int) arr, idx, (int) (4 * cnt));
        if (d)
            for (i = 0; i < cnt; i++) XfWrite(addr + i, be32(d + 4 * i));
        return 5;
    }
    if (op == 0x40) {
        unsigned addr, size;
        if (n < 9) return 0;
        addr = be32(p + 1);
        size = be32(p + 5);
        if (g_mem && g_dlDepth < 8 && (unsigned long long) addr + size <= g_memSize) {
            g_dlDepth++;
            ParseAll(g_mem + addr, size);
            g_dlDepth--;
        }
        return 9;
    }
    if (op == 0x61) {
        if (n < 5) return 0;
        BpWrite(be32(p + 1));
        return 5;
    }
    if (op >= 0x80) {
        unsigned count, vsz;
        if (n < 3) return 0;
        count = be16(p + 1);
        vsz = XGp_VertexSize((int) (op & 7));
        sz = 3 + count * vsz;
        *needed = sz;
        if (n < sz) return 0;
        Draw((int) op, (int) (op & 7), p + 3, count);
        return sz;
    }
    return (unsigned) -1;
}

// Parses a complete stream (display list); incomplete trailing data is dropped.
static unsigned ParseAll(const unsigned char* p, unsigned n)
{
    unsigned pos = 0, needed;
    while (pos < n) {
        unsigned c = ParseOne(p + pos, n - pos, &needed);
        if (c == 0) break;
        pos += c == (unsigned) -1 ? 1 : c;
        g_st.cmds++;
    }
    return pos;
}

extern "C" void XGp_Flush(void)
{
    unsigned needed;
    while (g_pos < g_len) {
        unsigned c;
        if (g_need && g_len - g_pos < g_need) break;
        g_need = 0;
        c = ParseOne(g_buf + g_pos, g_len - g_pos, &needed);
        if (c == 0) {
            g_need = needed;
            break;
        }
        g_pos += c == (unsigned) -1 ? 1 : c;
        g_st.cmds++;
    }
    if (g_pos == g_len) g_pos = g_len = 0;
    else if (g_pos > (1u << 16)) {
        memmove(g_buf, g_buf + g_pos, g_len - g_pos);
        g_len -= g_pos;
        g_pos = 0;
    }
}

extern "C" unsigned XGp_Pending(void) { return g_len - g_pos; }

extern "C" void XGp_Feed(const void* bytes, unsigned n)
{
    if (g_len + n > g_cap) {
        g_cap = (g_len + n) * 2 + 4096;
        g_buf = (unsigned char*) realloc(g_buf, g_cap);
    }
    memcpy(g_buf + g_len, bytes, n);
    g_len += n;
    XGp_Flush();
}

// SDK / game stores. The write-gather pipe feeds the CPU FIFO (PI registers 3 base, 4 top, 5 write pointer) in
// memory; when that FIFO is the one the command processor reads (CP base register == PI base: "linked" mode) the
// bytes are executed at once, otherwise (display list recording) they only accumulate in memory.
static unsigned short* CpRegs() { return (unsigned short*) (g_hw + 0x0000); }
static unsigned* PiRegs() { return (unsigned*) (g_hw + 0x3000); }

// The write-gather pipe collects 32-byte bursts: memory and the FIFO pointers only advance a whole burst at a time
// (the SDK's GXFlush pads with zeros to complete the last one). The processor itself is fed at once.
static unsigned char s_wg[32];
static unsigned s_wgn;

static void Burst(void)
{
    unsigned* pi = PiRegs();
    unsigned short* cp = CpRegs();
    unsigned base = pi[3], top = pi[4], wr = pi[5] & 0x03FFFFFFu;
    unsigned wrap = pi[5] & 0x04000000u;
    if (g_mem && (base || top)) {
        if (wr + 32 <= g_memSize) memcpy(g_mem + wr, s_wg, 32);
        wr += 32;
        if (wr > top + 3) {  // `top` is the address of the FIFO's last word (GXInitFifoBase: base + size - 4)
            wr = base;
            wrap ^= 0x04000000u;
        }
        pi[5] = wr | wrap;
        if ((unsigned) cp[0x20 / 2] == (base & 0xFFFF) && (unsigned) cp[0x22 / 2] == (base >> 16)) {
            cp[0x34 / 2] = cp[0x38 / 2] = (unsigned short) wr;  // write / read pointer: the processor is caught up
            cp[0x36 / 2] = cp[0x3A / 2] = (unsigned short) (wr >> 16);
            cp[0x30 / 2] = cp[0x32 / 2] = 0;
        }
    }
    s_wgn = 0;
}

static void Put(const unsigned char* b, unsigned n)
{
    unsigned* pi = PiRegs();
    unsigned short* cp = CpRegs();
    unsigned base = pi[3], i;
    int linked = (unsigned) cp[0x20 / 2] == (base & 0xFFFF) && (unsigned) cp[0x22 / 2] == (base >> 16);
    if (!g_mem || (!base && !pi[4])) {  // no FIFO set up yet: straight to the processor
        XGp_Feed(b, n);
        return;
    }
    for (i = 0; i < n; i++) {
        s_wg[s_wgn++] = b[i];
        if (s_wgn == 32) Burst();
    }
    if (linked) XGp_Feed(b, n);
}

extern "C" {
void XGx_W_u8(unsigned char v) { Put(&v, 1); }
void XGx_W_s8(signed char v) { unsigned char b = (unsigned char) v; Put(&b, 1); }
void XGx_W_u16(unsigned short v) { unsigned char b[2] = {(unsigned char) (v >> 8), (unsigned char) v}; Put(b, 2); }
void XGx_W_s16(short v) { XGx_W_u16((unsigned short) v); }
void XGx_W_u32(unsigned int v)
{
    unsigned char b[4] = {(unsigned char) (v >> 24), (unsigned char) (v >> 16), (unsigned char) (v >> 8), (unsigned char) v};
    Put(b, 4);
}
void XGx_W_s32(int v) { XGx_W_u32((unsigned) v); }
void XGx_W_f32(float f)
{
    unsigned u;
    memcpy(&u, &f, 4);
    XGx_W_u32(u);
}
}
