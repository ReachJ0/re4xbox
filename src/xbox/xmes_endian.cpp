// Big-endian -> little-endian conversion of message files (see include/xbox/xmes_endian.h).
#ifdef RE4_XBOX
#include "xbox/xmes_endian.h"
#include "xbox/xswap.h"

#define MES_MAX_BLOCKS 16

namespace {

struct Block {
    unsigned off, end;
};

// Reads the table without touching it; fills `b` with the ascending, distinct block ranges. Returns the table size
// in words (0 if invalid) and the block count in *nb.
unsigned Plan(const unsigned char* d, unsigned len, Block* b, int* nb)
{
    *nb = 0;
    if (len < 8) {
        return 0;
    }
    unsigned first = XSwap_Rd32(d + 4);
    if (first % 4 != 0 || first < 8 || first > len) {
        return 0;
    }
    unsigned words = first / 4;
    if (words - 1 > MES_MAX_BLOCKS) {
        return 0;
    }
    unsigned offs[MES_MAX_BLOCKS];
    int n = 0;
    for (unsigned i = 1; i < words; i++) {
        unsigned o = XSwap_Rd32(d + i * 4);
        if (o < first || o % 4 != 0 || o + 4 > len) {
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
    for (int i = 0; i < n; i++) {     // end of a block = the next larger offset, or the end of the file
        unsigned end = len;
        for (int k = 0; k < n; k++) {
            if (offs[k] > offs[i] && offs[k] < end) {
                end = offs[k];
            }
        }
        b[i].off = offs[i];
        b[i].end = end;
        unsigned cnt = XSwap_Rd32(d + offs[i]);
        if (cnt > (end - offs[i]) / 4 - 1) {   // count + ofs[count] must fit in the block
            return 0;
        }
    }
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
        unsigned cnt = XSwap_Rd32(d + b[i].off);
        unsigned hdr = (1 + cnt) * 4;
        for (unsigned w = 0; w < 1 + cnt; w++) {
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
