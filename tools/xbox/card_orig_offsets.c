/* Host-only helper for tools/xbox/card_test.cpp: exposes the SDK's own banner/icon offsets function.
 *
 * run_card_test.sh cuts `UpdateIconOffsets` out of src/lib/CARDStat.c into orig_update_offsets.inc and
 * compiles it here against a stub header made from the real include/dolphin/card.h constants and macros,
 * so the test compares the Xbox layer with the original code, not with a copy typed by hand. */
#include "orig_stub.h"
#include "orig_update_offsets.inc"

/* out[0..2] = stat bannerFormat, iconFormat, iconSpeed after the call
 * out[3] = offsetBanner, out[4] = offsetBannerTlut, out[5..12] = offsetIcon[0..7]
 * out[13] = offsetIconTlut, out[14] = offsetData */
void orig_offsets(unsigned iconAddr, unsigned bannerFormat, unsigned iconFormat, unsigned iconSpeed, unsigned* out)
{
    CARDDir ent;
    CARDStat stat;
    int i;

    ent.iconAddr = iconAddr;
    ent.bannerFormat = (unsigned char) bannerFormat;
    ent.iconFormat = (unsigned short) iconFormat;
    ent.iconSpeed = (unsigned short) iconSpeed;

    stat.bannerFormat = (unsigned char) bannerFormat;
    stat.iconFormat = (unsigned short) iconFormat;
    stat.iconSpeed = (unsigned short) iconSpeed;

    UpdateIconOffsets(&ent, &stat);

    out[0] = stat.bannerFormat;
    out[1] = stat.iconFormat;
    out[2] = stat.iconSpeed;
    out[3] = stat.offsetBanner;
    out[4] = stat.offsetBannerTlut;
    for (i = 0; i < 8; i++) {
        out[5 + i] = stat.offsetIcon[i];
    }
    out[13] = stat.offsetIconTlut;
    out[14] = stat.offsetData;
}
