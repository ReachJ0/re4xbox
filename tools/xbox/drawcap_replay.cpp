// Replays a draw capture written by the console (E:\drawcap.bin, see XRaster_SetCapture) with the host build of the
// software rasterizer and writes the result as a PPM.   usage: drawcap_replay drawcap.bin out.ppm [clear 0xAARRGGBB]
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox/xgp.h"
#include "xbox/xraster.h"

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s drawcap.bin out.ppm [clear argb]\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    unsigned h[10];
    if (fread(h, sizeof h, 1, f) != 1 || h[0] != 0x50414352u || h[1] != 1) { fprintf(stderr, "not a capture\n"); return 1; }
    int prim = (int) h[2], count = (int) h[3];
    // the console build is 32 bit: only the trailing 64 bit statistics of XGpState may differ in size, they are not needed
    if (h[5] != sizeof(XGpVertex) || h[4] < offsetof(XGpState, bpMask) + 4) { fprintf(stderr, "struct size mismatch %u/%u vs %zu/%zu\n", h[4], h[5], sizeof(XGpState), sizeof(XGpVertex)); return 1; }
    if (h[4] > sizeof(XGpState)) { fprintf(stderr, "state larger than expected\n"); return 1; }
    static XGpState st;
    XGpVertex* v = (XGpVertex*) calloc((size_t) count, sizeof(XGpVertex));
    size_t palBytes = (size_t) h[6] * h[7];
    unsigned char* pal = (unsigned char*) calloc(palBytes + 1, 1);
    if (fread(&st, 1, h[4] < sizeof st ? h[4] : sizeof st, f) != (h[4] < sizeof st ? h[4] : sizeof st) || fread(v, sizeof(XGpVertex), (size_t) count, f) != (size_t) count) return 1;
    if (palBytes && fread(pal, 1, palBytes, f) != palBytes) return 1;
    size_t memSize = h[8] ? h[8] : 1;
    unsigned char* mem = (unsigned char*) calloc(memSize, 1);
    unsigned used = h[9];
    for (int m = 0; m < 8; m++) {
        if (!(used >> m & 1)) continue;
        unsigned rec[3];
        if (fread(rec, sizeof rec, 1, f) != 1) return 1;
        printf("map %u: off %08x len %u\n", rec[0], rec[1], rec[2]);
        if (rec[2] && rec[1] + rec[2] <= memSize && fread(mem + rec[1], 1, rec[2], f) != rec[2]) return 1;
    }
    fclose(f);
    XGp_SetMemory(mem, (unsigned) memSize);
    XRaster_Init();
    XRaster_SetPalettes(pal, (int) h[7], h[6]);
    unsigned clear = argc > 3 ? (unsigned) strtoul(argv[3], 0, 0) : 0xFF000000u;
    XRaster_Clear(clear, 16777215.0f);
    const XGpBackend* be = XRaster_Backend();
    be->draw(prim, 0, v, count, &st, be->user);
    printf("prim %x, %d vertices, %llu triangles\n", prim, count, XRaster_Triangles());
    for (int i = 0; i < count && i < 6; i++) printf("v%d pos %.2f %.2f %.2f col %02x%02x%02x%02x tex %.3f %.3f\n", i, v[i].pos[0], v[i].pos[1], v[i].pos[2],
                                                     v[i].col[0][0], v[i].col[0][1], v[i].col[0][2], v[i].col[0][3], v[i].tex[0][0], v[i].tex[0][1]);
    printf("bp tev:");
    for (int k = 0; k < 6; k++) printf(" %06x", st.bp[0xC0 + k] & 0xFFFFFF);
    printf("\nbp 0x28 %06x 0x29 %06x ks %06x %06x blend %06x z %06x alpha %06x\n", st.bp[0x28], st.bp[0x29], st.bp[0xF6], st.bp[0xF7], st.bp[0x41], st.bp[0x40], st.bp[0xF3]);
    FILE* o = fopen(argv[2], "wb");
    if (!o) return 1;
    fprintf(o, "P6\n%d 480\n255\n", XRASTER_W);
    const unsigned* px = XRaster_Pixels();
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < XRASTER_W; x++) {
            unsigned p = px[y * XRASTER_W + x];
            fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o);
        }
    fclose(o);
    return 0;
}
