// Big-endian -> little-endian conversion of message files (see include/xbox/xmes_endian.h).
#ifdef RE4_XBOX
#include "xbox/xmes_endian.h"
#include "xbox/xswap.h"

#define MES_MAX_BLOCKS 16

// The first rejected file: { reason, length, first four words as stored (big-endian read as such) }, for the heartbeat.
extern "C" unsigned XMes_Diag[6];
unsigned XMes_Diag[6];

namespace {

void Reject(const unsigned char* d, unsigned len, unsigned why)
{
    if (XMes_Diag[0]) {
        return;   // keep the first failure
    }
    XMes_Diag[0] = why;
    XMes_Diag[1] = len;
    for (int i = 0; i < 4; i++) {
        XMes_Diag[2 + i] = (unsigned) (i * 4 + 4 <= (int) len ? XSwap_Rd32(d + i * 4) : 0xFFFFFFFFu);
    }
}

struct Block {
    unsigned off, end;
};

// Reads the table without touching it; fills `b` with the ascending, distinct block ranges. Returns the table size
// in words (0 if invalid) and the block count in *nb.
unsigned Plan(const unsigned char* d, unsigned len, Block* b, int* nb)
{
    *nb = 0;
    if (len < 8) {
        Reject(d, len, 1);
        return 0;
    }
    unsigned first = XSwap_Rd32(d + 4);
    if (first % 4 != 0 || first < 8 || first > len) {
        Reject(d, len, 2);
        return 0;
    }
    unsigned words = first / 4;
    if (words - 1 > MES_MAX_BLOCKS) {
        Reject(d, len, 3);
        return 0;
    }
    unsigned offs[MES_MAX_BLOCKS];
    int n = 0;
    for (unsigned i = 1; i < words; i++) {
        unsigned o = XSwap_Rd32(d + i * 4);
        if (o < first || o % 4 != 0 || o + 4 > len) {
            Reject(d, len, 4);
            return 0;
        }
        int dup = 0;
        for (int k = 0; k < n; k++) {
            dup |= offs[k] == o;
        }
        if (!dup) {
            offs[n++] = o;
        }
    }
    int valid = 0;
    for (int i = 0; i < n; i++) {     // end of a block = the next larger offset, or the end of the file
        unsigned end = len;
        for (int k = 0; k < n; k++) {
            if (offs[k] > offs[i] && offs[k] < end) {
                end = offs[k];
            }
        }
        if (end - offs[i] < 8) {
            continue;
        }
        unsigned cnt = XSwap_Rd32(d + offs[i] + 4);   // block = { u32 x0; u32 count; u32 ofs[count]; text }
        if (cnt > (end - offs[i]) / 4 - 2) {   // x0 + count + ofs[count] must fit in the block
            // Not a message block (the disc keeps unused/other-format data in some language slots): leave it as it is
            // and convert the rest; the diagnostic keeps the first one: offset, end, count, index.
            unsigned was = XMes_Diag[0];
            Reject(d, len, 5);
            if (was == 0) {
                XMes_Diag[2] = offs[i];
                XMes_Diag[3] = end;
                XMes_Diag[4] = cnt;
                XMes_Diag[5] = (unsigned) i;
            }
            continue;
        }
        b[valid].off = offs[i];
        b[valid].end = end;
        valid++;
    }
    n = valid;
    *nb = n;
    return words;
}

}  // namespace

extern "C" int XMes_Swap(void* data, unsigned len)
{
    unsigned char* d = (unsigned char*) data;
    Block b[MES_MAX_BLOCKS];
    int nb;
    unsigned words = Plan(d, len, b, &nb);
    if (!words || nb == 0) {
        return 0;
    }
    for (int i = 0; i < nb; i++) {
        unsigned cnt = XSwap_Rd32(d + b[i].off + 4);
        unsigned hdr = (2 + cnt) * 4;
        for (unsigned w = 0; w < 2 + cnt; w++) {
            XSwap_32(d + b[i].off + w * 4);
        }
        unsigned char* t = d + b[i].off + hdr;
        unsigned n = (b[i].end - b[i].off - hdr) / 2;
        for (unsigned k = 0; k < n; k++) {
            XSwap_16(t + k * 2);
        }
    }
    for (unsigned i = 0; i < words; i++) {
        XSwap_32(d + i * 4);
    }
    return nb;
}
#endif
