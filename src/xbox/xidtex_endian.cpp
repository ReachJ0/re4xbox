// Conversion of the id data files (see include/xbox/xidtex_endian.h). Layouts: include/texture.h (IdTexData, TexIdTbl,
// TexOfsTbl, TexAnm/cAnm), include/id_sys.h (ID_FILE_HEADER, ID_DATA_V1/V2), include/hermite.h, include/path.h.
// Every offset is checked against the length of its block; whatever is out of range is left alone and reported.
#include "xbox/xidtex_endian.h"
#include "xbox/xtex_endian.h"
#include "xbox/xswap.h"

static inline unsigned char* P(void* d, unsigned o) { return (unsigned char*) d + o; }

// Swaps `n` 32-bit words at byte offset `o`; 0 if they do not fit.
static int Words(void* d, unsigned len, unsigned o, unsigned n)
{
    if (o > len || n > (len - o) / 4) return 0;
    for (unsigned i = 0; i < n; i++) XSwap_32(P(d, o + i * 4));
    return 1;
}

extern "C" int XIdTex_Swap(void* data, unsigned len)
{
    if (len < 0x20 || XSwap_Rd32(P(data, 0)) != 0xB) return 0;
    XSwap_32(P(data, 0));
    unsigned ofsId = XSwap_32(P(data, 4));
    unsigned ofsTpl = XSwap_32(P(data, 0x18));
    unsigned ofsAnm = XSwap_32(P(data, 0x1C));
    int ok = 1;

    // id table { u32 num; { u16 id; u16 x2; u32 x4 }[num] }
    if (ofsId > len || len - ofsId < 4) return 0;
    unsigned num = XSwap_32(P(data, ofsId));
    if (num > 0x200 || (len - ofsId - 4) / 8 < num) return 0;
    for (unsigned i = 0; i < num; i++) {
        unsigned char* e = P(data, ofsId + 4 + i * 8);
        XSwap_16(e);
        XSwap_16(e + 2);
        XSwap_32(e + 4);
    }

    // the two offset tables { u32 num; u32 ofs[num] }, relative to the table
    if (ofsTpl > len || ofsAnm > len || len - ofsTpl < 4 || len - ofsAnm < 4) return 0;
    unsigned nTpl = XSwap_32(P(data, ofsTpl));
    unsigned nAnm = XSwap_32(P(data, ofsAnm));
    if (nTpl < num || nAnm < num || nTpl > 0x200 || nAnm > 0x200 || (len - ofsTpl - 4) / 4 < nTpl || (len - ofsAnm - 4) / 4 < nAnm) return 0;
    for (unsigned i = 0; i < nTpl; i++) XSwap_32(P(data, ofsTpl + 4 + i * 4));
    for (unsigned i = 0; i < nAnm; i++) XSwap_32(P(data, ofsAnm + 4 + i * 4));

    unsigned seenTpl[0x200], seenAnm[0x200];
    unsigned nsT = 0, nsA = 0;
    for (unsigned i = 0; i < num; i++) {
        unsigned t = ofsTpl + *(unsigned*) P(data, ofsTpl + 4 + i * 4);   // offsets are native now
        unsigned a = ofsAnm + *(unsigned*) P(data, ofsAnm + 4 + i * 4);
        bool dup = false;
        for (unsigned k = 0; k < nsT; k++) if (seenTpl[k] == t) dup = true;
        if (!dup && t < len) {
            seenTpl[nsT++] = t;
            if (!XTex_SwapTpl(P(data, t), len - t)) ok = 0;
        } else if (!dup) ok = 0;
        dup = false;
        for (unsigned k = 0; k < nsA; k++) if (seenAnm[k] == a) dup = true;
        if (!dup && a < len && len - a >= 0x10) {
            seenAnm[nsA++] = a;
            for (int k = 0; k < 5; k++) XSwap_16(P(data, a + k * 2));   // Width Height Cx Cy Frames
        } else if (!dup) ok = 0;
    }
    return ok;
}

extern "C" int XIdData_Swap(void* data, unsigned len)
{
    if (len < 8) return 0;
    const unsigned char* h = (const unsigned char*) data;
    int ver = h[0] - '0';
    if (ver != 1 && ver != 2) return 0;
    unsigned units = h[5];
    unsigned esz = ver == 1 ? 0x88 : 0x8C;
    unsigned rot = ver == 1 ? 0x5C : 0x60;
    unsigned ofs = ver == 1 ? 0x70 : 0x74;
    if (len - 8 < units * esz) return 0;
    int ok = 1;
    for (unsigned i = 0; i < units; i++) {
        unsigned b = 8 + i * esz;
        Words(data, len, b + 0x14, 3);          // pos
        Words(data, len, b + 0x20, 12);         // vtx[4]
        Words(data, len, b + 0x50, 2);          // sizeX sizeY
        Words(data, len, b + rot, 3);           // rot
        Words(data, len, b + ofs, 6);           // path0 path1 curve[4]
    }
    // curves / paths (offsets from the table start; a block shared by several units is converted once)
    unsigned done[0x100];
    unsigned nd = 0;
    for (unsigned i = 0; i < units; i++) {
        unsigned b = 8 + i * esz;
        for (int k = 0; k < 6; k++) {
            unsigned o = *(unsigned*) P(data, b + ofs + k * 4);
            if (!o || k == 1) continue;           // path1 is the work area FuncPathParametrize fills
            bool dup = false;
            for (unsigned q = 0; q < nd; q++) if (done[q] == o) dup = true;
            if (dup) continue;
            if (nd < 0x100) done[nd++] = o;
            if (o > len || len - o < 8) { ok = 0; continue; }
            if (k == 0) {                          // FUNC_PATH_PTR { s8 order; u8 pad[6]; s8 nPoint; Vec[nPoint] }
                unsigned n = h[o + 7];
                if (!Words(data, len, o + 8, n * 3)) ok = 0;
            } else {                               // HERMITE_1_PTR { s32 n; { f32 T,Q,dQ[2] }[n] }
                unsigned n = XSwap_32(P(data, o));
                if (n > 64 || !Words(data, len, o + 4, n * 4)) ok = 0;
            }
        }
    }
    return ok;
}

extern "C" int XIdArc_Swap(void* arc, unsigned len, unsigned texMask, unsigned idMask)
{
    const unsigned cap = 0x4000000;
    if (!len || len > cap) len = cap;
    unsigned ofs[16];
    for (int i = 0; i < 16; i++) ofs[i] = XSwap_32(P(arc, i * 4));
    int bad = 0;
    for (int i = 0; i < 16; i++) {
        if (!(((texMask | idMask) >> i) & 1)) continue;
        unsigned o = ofs[i];
        if (o < 0x40 || o >= len) { bad++; continue; }
        unsigned end = len;
        for (int k = 0; k < 16; k++) if (ofs[k] > o && ofs[k] < end) end = ofs[k];
        int r = ((texMask >> i) & 1) ? XIdTex_Swap(P(arc, o), end - o) : XIdData_Swap(P(arc, o), end - o);
        if (!r) bad++;
    }
    return bad;
}
