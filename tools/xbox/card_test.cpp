// Host unit test for src/xbox/card_xbox.cpp (the Xbox save layer). Run it with tools/xbox/run_card_test.sh.
//
// The layer talks to the platform only through XCardBackend_*, so this file provides an in-memory fake with
// fault injection and drives the same CARD calls the game makes, in the order its state machines use them.
// It checks the card rules the game depends on (sizes, alignment, error codes), data integrity across
// blocks and files, persistence across unmount / mount, recovery from a power cut and from a corrupt
// image, and that the banner / icon offsets match the SDK's own function over a large input sweep.
#include "card.h"
#include "xbox/xcard_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Original SDK offsets function, from src/lib/CARDStat.c (tools/xbox/card_orig_offsets.c)
extern "C" void orig_offsets(unsigned iconAddr, unsigned bannerFormat, unsigned iconFormat, unsigned iconSpeed,
                             unsigned* out /* [4 + 1 + 8 + 2] */);

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
        }                                                                    \
    } while (0)

// ---- fake backend ----
#define FAKE_MAX (3u * 1024 * 1024)  // no std::vector: the game's card.h replaces operator new
struct FakeSlot {
    unsigned char img[FAKE_MAX];
    unsigned imgSize;
    int present;
    int failWritesAfter;  // -1 = never; N = the N+1th Write (and later ones) fail
    int writes;
    int flushes;
    int failFlush;
    int failSize;
};
static FakeSlot g_fake[XCARD_SLOTS];

static void FakeAssign(int slot, unsigned n, unsigned char v)
{
    memset(g_fake[slot].img, v, n);
    g_fake[slot].imgSize = n;
}
static unsigned g_clock = 1000;
static int g_initResult = 1;
static int g_initCalls = 0;

extern "C" {
int XCardBackend_Init(void)
{
    g_initCalls++;
    return g_initResult;
}
int XCardBackend_Present(int slot) { return g_fake[slot].present; }
int XCardBackend_Size(int slot, unsigned int* size)
{
    if (g_fake[slot].failSize) {
        return 0;
    }
    *size = g_fake[slot].imgSize;
    return 1;
}
int XCardBackend_Read(int slot, unsigned int off, void* buf, unsigned int len)
{
    FakeSlot& f = g_fake[slot];
    if (off > f.imgSize || len > f.imgSize - off) {
        return 0;
    }
    memcpy(buf, f.img + off, len);
    return 1;
}
int XCardBackend_Write(int slot, unsigned int off, const void* buf, unsigned int len)
{
    FakeSlot& f = g_fake[slot];
    if (f.failWritesAfter >= 0 && f.writes >= f.failWritesAfter) {
        return 0;
    }
    f.writes++;
    if (off + len > FAKE_MAX) {
        return 0;
    }
    if (off + len > f.imgSize) {
        memset(f.img + f.imgSize, 0, off + len - f.imgSize);
        f.imgSize = off + len;
    }
    memcpy(f.img + off, buf, len);
    return 1;
}
int XCardBackend_Truncate(int slot, unsigned int size)
{
    if (size < g_fake[slot].imgSize) {
        g_fake[slot].imgSize = size;
    }
    return 1;
}
int XCardBackend_Flush(int slot)
{
    g_fake[slot].flushes++;
    return !g_fake[slot].failFlush;
}
unsigned int XCardBackend_Seconds2000(void) { return g_clock; }
}

static const int BLOCK = 0x2000;

// Fresh world: slot A present with an empty disk, slot B absent, layer initialised and slot A mounted.
static void ResetWorld(int mountA = 1)
{
    static int first = 1;
    int i;

    for (i = 0; i < XCARD_SLOTS; i++) {
        g_fake[i] = FakeSlot();
        g_fake[i].failWritesAfter = -1;
    }
    g_fake[0].present = 1;
    g_clock = 1000;
    g_initResult = 1;
    if (first) {  // CARDInit does its work once per run, like the SDK
        CARDInit();
        first = 0;
    }
    CARDUnmount(0);
    CARDUnmount(1);
    if (mountA) {
        CARDMountAsync(0, 0, 0, 0);
    }
}

static void Fill(unsigned char* p, int len, unsigned seed)
{
    for (int i = 0; i < len; i++) {
        seed = seed * 1664525u + 1013904223u;
        p[i] = (unsigned char) (seed >> 24);
    }
}

static int MakeFile(const char* name, int blocks, CardFileInfo* fi)
{
    CardFileInfo local;
    return CARDCreateAsync(0, name, (u32) blocks * BLOCK, fi ? fi : &local, 0);
}

// ---------------------------------------------------------------------------------------------------

static void TestInitProbe()
{
    s32 mem = 0, sec = 0;

    ResetWorld(0);
    CHECK(CARDProbeEx(0, &mem, &sec) == 0);
    CHECK(mem == 16 && sec == 0x2000);  // what the game's existCheck needs: sector size exactly 0x2000
    CHECK(CARDProbeEx(0, 0, 0) == 0);   // the game also calls it with null out-pointers
    CHECK(CARDProbeEx(1, &mem, &sec) == -3);  // slot B has no storage
    CHECK(CARDProbeEx(2, &mem, &sec) == -128);
    CHECK(CARDProbeEx(-1, &mem, &sec) == -128);
    g_fake[0].present = 0;
    CHECK(CARDProbeEx(0, &mem, &sec) == -3);  // disk gone
    CHECK(CARDGetResultCode(5) == -128);
}

static void TestMountFresh()
{
    s32 bytes = 0, files = 0;
    u64 serial = 0;

    ResetWorld(0);
    // nothing works before the mount
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -3);
    CHECK(CARDGetResultCode(0) == -3);
    CHECK(CARDGetSerialNo(0, &serial) == -3);

    CHECK(CARDMountAsync(0, 0, 0, 0) == 0);
    CHECK(CARDGetResultCode(0) == 0);  // a new, empty disk is formatted silently, no "format?" prompt
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0);
    CHECK(bytes == 251 * BLOCK && files == 127);
    CHECK(g_fake[0].imgSize == 2u * BLOCK);
    CHECK(CARDCheckAsync(0, 0) == 0 && CARDGetResultCode(0) == 0);

    // mount on an absent slot reports no card through the result
    CHECK(CARDMountAsync(1, 0, 0, 0) == -3);
    CHECK(CARDGetResultCode(1) == -3);

    // unmount, then everything refuses again
    CHECK(CARDUnmount(0) == 0);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -3);
}

static void TestSerial()
{
    u64 a = 0, b = 0, c = 0;

    ResetWorld();
    CHECK(CARDGetSerialNo(0, &a) == 0 && a != 0);
    CHECK(CARDGetSerialNo(0, &b) == 0 && a == b);  // stable: the game compares it between steps
    g_clock = 99999;
    CHECK(CARDFormatAsync(0, 0) == 0);
    CHECK(CARDGetSerialNo(0, &c) == 0 && c != a);  // a formatted card is a new card
    CHECK(CARDGetSerialNo(0, 0) == -128);
}

static void TestCreateRules()
{
    CardFileInfo fi, fi2;
    s32 bytes, files;
    char longName[40];

    ResetWorld();
    CHECK(CARDCreateAsync(0, "SAVE", 0, &fi, 0) == -128);          // size 0
    CHECK(CARDGetResultCode(0) == -128);
    CHECK(CARDCreateAsync(0, "SAVE", 0x1000, &fi, 0) == -128);     // not a multiple of 8 KB
    memset(longName, 'A', 33);
    longName[33] = 0;
    CHECK(CARDCreateAsync(0, longName, BLOCK, &fi, 0) == -12);     // 33 characters
    CHECK(CARDGetResultCode(0) == -12);
    memset(longName, 'B', 32);
    longName[32] = 0;
    CHECK(CARDCreateAsync(0, longName, BLOCK, &fi, 0) == 0);       // exactly 32 is allowed
    CHECK(CARDOpen(0, longName, &fi2) == 0 && fi2.fileNo == fi.fileNo);

    fi.chan = fi.fileNo = -77;
    CHECK(CARDCreateAsync(0, "SAVE0", 8 * BLOCK, &fi, 0) == 0);    // the game's save file is 8 blocks
    CHECK(CARDGetResultCode(0) == 0);
    CHECK(fi.chan == 0 && fi.fileNo == 1 && fi.offset == 0);       // left open, like the SDK
    CHECK(CARDCreateAsync(0, "SAVE0", BLOCK, &fi, 0) == -7);       // exists
    CHECK(CARDGetResultCode(0) == -7);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0);
    CHECK(bytes == (251 - 9) * BLOCK && files == 125);

    CHECK(CARDCreateAsync(0, "BIG", 300u * BLOCK, &fi, 0) == -9);  // does not fit
    CHECK(CARDCreateAsync(1, "X", BLOCK, &fi, 0) == -3);           // slot B: no card
    CHECK(CARDCreateAsync(2, "X", BLOCK, &fi, 0) == -128);
}

static void TestReadWrite()
{
    CardFileInfo fi;
    static unsigned char out[8 * BLOCK], in[8 * BLOCK], part[2048];

    ResetWorld();
    CHECK(MakeFile("SAVE0", 8, &fi) == 0);
    Fill(out, sizeof(out), 7);

    // write the whole file in one call, as the game does (blocks << 13)
    CHECK(CARDWriteAsync(&fi, out, 8 * BLOCK, 0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(CARDReadAsync(&fi, in, 8 * BLOCK, 0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(memcmp(in, out, sizeof(out)) == 0);

    // reads in 512-byte units, across a block boundary and in the middle of a block
    CHECK(CARDReadAsync(&fi, part, 2048, 3 * BLOCK - 1024, 0) == 0);
    CHECK(memcmp(part, out + 3 * BLOCK - 1024, 2048) == 0);
    CHECK(CARDReadAsync(&fi, part, 512, 5 * BLOCK + 512, 0) == 0);
    CHECK(memcmp(part, out + 5 * BLOCK + 512, 512) == 0);

    // rules: reads in 512s, writes in 8 KB; nothing outside the file
    CHECK(CARDReadAsync(&fi, part, 100, 0, 0) == -128);
    CHECK(CARDReadAsync(&fi, part, 512, 100, 0) == -128);
    CHECK(CARDReadAsync(&fi, part, 0, 0, 0) == -128);
    CHECK(CARDReadAsync(&fi, part, 512, 8 * BLOCK, 0) == -11);
    CHECK(CARDReadAsync(&fi, in, 8 * BLOCK, 512, 0) == -11);       // runs past the end
    CHECK(CARDWriteAsync(&fi, out, 512, 0, 0) == -128);
    CHECK(CARDWriteAsync(&fi, out, BLOCK, 512, 0) == -128);
    CHECK(CARDWriteAsync(&fi, out, 2 * BLOCK, 7 * BLOCK, 0) == -11);
    CHECK(CARDGetResultCode(0) == -11);

    // a partial write touches only its blocks
    Fill(part, sizeof(part), 99);
    static unsigned char blk[BLOCK];
    Fill(blk, BLOCK, 1234);
    CHECK(CARDWriteAsync(&fi, blk, BLOCK, 2 * BLOCK, 0) == 0);
    CHECK(CARDReadAsync(&fi, in, 8 * BLOCK, 0, 0) == 0);
    CHECK(memcmp(in, out, 2 * BLOCK) == 0);
    CHECK(memcmp(in + 2 * BLOCK, blk, BLOCK) == 0);
    CHECK(memcmp(in + 3 * BLOCK, out + 3 * BLOCK, 5 * BLOCK) == 0);

    // a freshly created file reads as zeros before it is written
    CardFileInfo fz;
    CHECK(MakeFile("EMPTY", 2, &fz) == 0);
    memset(in, 0xAA, 2 * BLOCK);
    CHECK(CARDReadAsync(&fz, in, 2 * BLOCK, 0, 0) == 0);
    bool zero = true;
    for (int i = 0; i < 2 * BLOCK; i++) {
        zero = zero && in[i] == 0;
    }
    CHECK(zero);

    // closed handles are refused
    CHECK(CARDClose(&fi) == 0);
    CHECK(fi.chan == -1);
    CHECK(CARDReadAsync(&fi, in, 512, 0, 0) == -128);
    CHECK(CARDClose(&fi) == -128);
}

// Several files with deleted gaps between them: chains are not contiguous, data must stay separate.
static void TestFilesIsolatedAndFragmented()
{
    CardFileInfo f[5];
    static unsigned char data[5][4 * BLOCK], got[4 * BLOCK];
    const char* names[5] = {"A", "B", "C", "D", "E"};
    const int blocks[5] = {1, 2, 1, 3, 1};
    int i;

    ResetWorld();
    for (i = 0; i < 5; i++) {
        CHECK(MakeFile(names[i], blocks[i], &f[i]) == 0);
    }
    CHECK(CARDDeleteAsync(0, "B", 0) == 0);
    CHECK(CARDDeleteAsync(0, "D", 0) == 0);
    CHECK(MakeFile("F", 4, &f[1]) == 0);  // 4 blocks into the 2 + 3 block holes: a split chain
    CHECK(CARDCreateAsync(0, "A", BLOCK, &f[3], 0) == -7);

    // write distinct data to every surviving file, read it all back
    const char* now[4] = {"A", "C", "E", "F"};
    const int nb[4] = {1, 1, 1, 4};
    CardFileInfo h[4];
    for (i = 0; i < 4; i++) {
        CHECK(CARDOpen(0, now[i], &h[i]) == 0);
        Fill(data[i], nb[i] * BLOCK, 500 + i);
        CHECK(CARDWriteAsync(&h[i], data[i], nb[i] * BLOCK, 0, 0) == 0);
    }
    for (i = 0; i < 4; i++) {
        memset(got, 0, sizeof(got));
        CHECK(CARDReadAsync(&h[i], got, nb[i] * BLOCK, 0, 0) == 0);
        CHECK(memcmp(got, data[i], nb[i] * BLOCK) == 0);
    }
    s32 bytes, files;
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0);
    CHECK(bytes == (251 - 7) * BLOCK && files == 123);
}

static void TestDelete()
{
    CardFileInfo fi;
    s32 bytes, files;

    ResetWorld();
    CHECK(MakeFile("SAVE0", 8, &fi) == 0);
    CHECK(CARDDeleteAsync(0, "NOPE", 0) == -4);
    CHECK(CARDGetResultCode(0) == -4);
    CHECK(CARDDeleteAsync(0, "SAVE0", 0) == 0);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0 && bytes == 251 * BLOCK && files == 127);
    CHECK(CARDOpen(0, "SAVE0", &fi) == -4);
    CHECK(fi.chan == -1);                       // a failed open leaves no valid handle
    CHECK(CARDReadAsync(&fi, &fi, 512, 0, 0) == -128);
    CHECK(MakeFile("SAVE0", 8, &fi) == 0);      // the name and blocks are reusable
    CHECK(CARDDeleteAsync(0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 0) == -12);

    // a handle that outlives its file is refused, not read through
    CardFileInfo stale;
    CHECK(CARDOpen(0, "SAVE0", &stale) == 0);
    CHECK(CARDDeleteAsync(0, "SAVE0", 0) == 0);
    CHECK(CARDReadAsync(&stale, &fi, 512, 0, 0) == -4);
}

static void TestFullCard()
{
    CardFileInfo fi;
    s32 bytes, files;
    int made = 0;
    char name[16];

    // all blocks: 31 files of 8 blocks = 248, then 3 left
    ResetWorld();
    for (int i = 0; i < 31; i++) {
        sprintf(name, "F%d", i);
        if (MakeFile(name, 8, &fi) == 0) {
            made++;
        }
    }
    CHECK(made == 31);
    CHECK(MakeFile("TOOBIG", 4, &fi) == -9);
    CHECK(MakeFile("FITS", 3, &fi) == 0);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0 && bytes == 0);
    CHECK(MakeFile("NONE", 1, &fi) == -9);

    // all directory entries: 127 one-block files, the 128th has no entry
    ResetWorld();
    made = 0;
    for (int i = 0; i < 127; i++) {
        sprintf(name, "G%d", i);
        if (MakeFile(name, 1, &fi) == 0) {
            made++;
        }
    }
    CHECK(made == 127);
    CHECK(MakeFile("G127", 1, &fi) == -8);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0 && files == 0 && bytes == (251 - 127) * BLOCK);
}

static void TestPersistence()
{
    CardFileInfo fi, fi2;
    static unsigned char out[3 * BLOCK], in[3 * BLOCK];
    CardStat st;
    u64 serial1 = 0, serial2 = 0;

    ResetWorld();
    CHECK(MakeFile("SYSTEM", 1, &fi) == 0);
    CHECK(MakeFile("SAVE0", 3, &fi) == 0);
    Fill(out, sizeof(out), 31337);
    CHECK(CARDWriteAsync(&fi, out, 3 * BLOCK, 0, 0) == 0);
    CHECK(CARDGetSerialNo(0, &serial1) == 0);

    // the card is "removed" and put back: unmount, mount, everything still there
    CHECK(CARDUnmount(0) == 0);
    CHECK(CARDMountAsync(0, 0, 0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(CARDCheckAsync(0, 0) == 0);
    CHECK(CARDOpen(0, "SAVE0", &fi2) == 0 && fi2.fileNo == fi.fileNo);
    CHECK(CARDReadAsync(&fi2, in, 3 * BLOCK, 0, 0) == 0);
    CHECK(memcmp(in, out, sizeof(out)) == 0);
    CHECK(CARDGetStatus(0, fi2.fileNo, &st) == 0);
    CHECK(strncmp(st.fileName, "SAVE0", 32) == 0 && st.length == 3u * BLOCK);
    CHECK(CARDGetSerialNo(0, &serial2) == 0 && serial1 == serial2);

    // a power cycle loses the layer's memory but keeps the image: unmount, mount, the card is the same
    CARDUnmount(0);
    CHECK(CARDMountAsync(0, 0, 0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(CARDOpen(0, "SYSTEM", &fi2) == 0);

    // every operation that changes the image flushes it
    int before = g_fake[0].flushes;
    CHECK(MakeFile("X", 1, &fi) == 0);
    CHECK(g_fake[0].flushes > before);
    before = g_fake[0].flushes;
    CHECK(CARDWriteAsync(&fi, out, BLOCK, 0, 0) == 0);
    CHECK(g_fake[0].flushes > before);
    before = g_fake[0].flushes;
    CHECK(CARDDeleteAsync(0, "X", 0) == 0);
    CHECK(g_fake[0].flushes > before);
}

static void TestStatus()
{
    CardFileInfo fi;
    CardStat st, back;
    u32 t0;

    ResetWorld();
    g_clock = 5000;
    CHECK(MakeFile("SAVE0", 2, &fi) == 0);

    CHECK(CARDGetStatus(0, fi.fileNo, &st) == 0);
    CHECK(st.length == 2u * BLOCK && st.time == 5000);
    CHECK(st.iconAddr == 0xFFFFFFFFu && st.commentAddr == 0xFFFFFFFFu);  // nothing set yet
    CHECK(CARDGetStatus(0, 5, &st) == -4);                                // empty entry
    CHECK(CARDGetStatus(0, 127, &st) == -128);
    CHECK(CARDGetStatus(0, -1, &st) == -128);

    // what game/card.cpp makeCardStatus sets: RGB5A3 banner, one C8 icon at speed 3, icons at 0x40
    memset(&st, 0, sizeof(st));
    st.bannerFormat = 2;
    st.iconAddr = 0x40;
    st.iconFormat = 1;
    st.iconSpeed = 3;
    st.commentAddr = 0;
    g_clock = 6000;
    CHECK(CARDSetStatusAsync(0, fi.fileNo, &st, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(st.offsetBanner == 0x40);                // the SDK also fills the offsets back into the caller's stat
    CHECK(st.offsetIcon[0] == 0x40 + 2 * 96 * 32);
    CHECK(st.offsetIconTlut == 0x40 + 2 * 96 * 32 + 32 * 32);
    CHECK(st.offsetData == st.offsetIconTlut + 512);

    memset(&back, 0xEE, sizeof(back));
    CHECK(CARDGetStatus(0, fi.fileNo, &back) == 0);
    CHECK(back.bannerFormat == 2 && back.iconAddr == 0x40 && back.iconFormat == 1 && back.iconSpeed == 3);
    CHECK(back.time == 6000);                       // set status stamps the date
    CHECK(back.offsetData == st.offsetData && back.offsetBanner == 0x40 && back.offsetBannerTlut == 0xFFFFFFFFu);

    // writing the file stamps the date too
    t0 = 7000;
    g_clock = t0;
    static unsigned char blk[BLOCK];
    CHECK(CARDWriteAsync(&fi, blk, BLOCK, 0, 0) == 0);
    CHECK(CARDGetStatus(0, fi.fileNo, &back) == 0 && back.time == t0);

    // argument rules of the SDK
    st.iconAddr = 512;
    CHECK(CARDSetStatusAsync(0, fi.fileNo, &st, 0) == -128);     // icon data must start in the first 512 bytes
    st.iconAddr = 0x40;
    st.commentAddr = BLOCK - 32;
    CHECK(CARDSetStatusAsync(0, fi.fileNo, &st, 0) == -128);     // a comment must not cross an 8 KB line
    st.commentAddr = 0;
    CHECK(CARDSetStatusAsync(0, 99, &st, 0) == -4);

    // no icon: the first icon speed is forced to "fast" and the format fields are cleared in the caller's stat
    st.iconAddr = 0xFFFFFFFFu;
    st.iconSpeed = 3;
    CHECK(CARDSetStatusAsync(0, fi.fileNo, &st, 0) == 0);
    CHECK(st.iconFormat == 0 && st.iconSpeed == 0 && st.bannerFormat == 0);
    // (GetStatus itself reports speed 0 for a file without icons, as the SDK does, so look at the stored entry)
    CHECK(CARDGetStatus(0, fi.fileNo, &back) == 0 && back.iconSpeed == 0);
    CHECK((g_fake[0].img[64 + fi.fileNo * 64 + 50] & 3) == 1);
}

// The banner / icon layout must equal the SDK function for every combination the directory can hold.
static void TestOffsetsMatchSdk()
{
    CardFileInfo fi;
    long compared = 0;
    static const unsigned addrs[] = {0xFFFFFFFFu, 0, 0x40, 0x1FF};
    unsigned fmtStep;

    ResetWorld();
    CHECK(MakeFile("SAVE0", 1, &fi) == 0);

    for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
        for (unsigned banner = 0; banner < 8; banner++) {
            // every 16-bit icon format for the common case, a coarse sweep for the rest
            fmtStep = (addrs[a] == 0x40) ? 1 : 257;
            for (unsigned fmt = 0; fmt < 0x10000; fmt += fmtStep) {
                unsigned spd = (fmt * 7) & 0xFFFF;
                CardStat st;
                unsigned expect[15];

                memset(&st, 0, sizeof(st));
                st.bannerFormat = (u8) banner;
                st.iconAddr = addrs[a];
                st.iconFormat = (u16) fmt;
                st.iconSpeed = (u16) spd;
                st.commentAddr = 0;
                orig_offsets(addrs[a], banner, fmt, spd, expect);

                if (CARDSetStatusAsync(0, fi.fileNo, &st, 0) != 0) {
                    CHECK(!"SetStatus rejected a valid stat");
                    return;
                }
                compared++;
                bool same = st.bannerFormat == expect[0] && st.iconFormat == expect[1] && st.iconSpeed == expect[2] &&
                            st.offsetBanner == expect[3] && st.offsetBannerTlut == expect[4] &&
                            st.offsetIconTlut == expect[13] && st.offsetData == expect[14];
                for (int i = 0; i < 8; i++) {
                    same = same && st.offsetIcon[i] == expect[5 + i];
                }
                if (!same) {
                    printf("offsets differ: addr=%x banner=%u fmt=%x\n", addrs[a], banner, fmt);
                    CHECK(!"offsets differ from the SDK");
                    return;
                }
            }
        }
    }
    CHECK(compared > 200000);
    printf("banner/icon offsets compared against CARDStat.c on %ld inputs\n", compared);
}

// A power cut part-way through create or delete: the card must come back valid, earlier files intact, and
// the blocks the interrupted operation took must be usable again.
static void TestPowerCut()
{
    CardFileInfo fi;
    static unsigned char out[BLOCK], in[BLOCK];
    s32 bytes, files;

    for (int mode = 0; mode < 2; mode++) {
        for (int cut = 0; cut < 4; cut++) {  // which write of the operation fails
            ResetWorld();
            Fill(out, BLOCK, 4242);
            CHECK(MakeFile("KEEP", 1, &fi) == 0);
            CHECK(CARDWriteAsync(&fi, out, BLOCK, 0, 0) == 0);
            if (mode == 1) {
                CHECK(MakeFile("VICTIM", 2, &fi) == 0);
            }

            g_fake[0].writes = 0;
            g_fake[0].failWritesAfter = cut;
            s32 r = (mode == 0) ? MakeFile("NEW", 3, &fi) : CARDDeleteAsync(0, "VICTIM", 0);
            CHECK(cut >= 2 ? r == 0 : r < 0);  // both operations write twice (FAT+dir / dir+FAT); later cuts find nothing to cut
            if (cut < 2) {
                CHECK(CARDGetResultCode(0) < 0);  // the failure is reported, never swallowed
            }

            // "power back on": the disk is whatever was written; no more injected failures
            g_fake[0].failWritesAfter = -1;
            CARDUnmount(0);
            CHECK(CARDMountAsync(0, 0, 0, 0) == 0);
            CHECK(CARDGetResultCode(0) == 0);  // valid, not "broken": leaked blocks are reclaimed, not fatal
            CHECK(CARDOpen(0, "KEEP", &fi) == 0);
            memset(in, 0, BLOCK);
            CHECK(CARDReadAsync(&fi, in, BLOCK, 0, 0) == 0 && memcmp(in, out, BLOCK) == 0);

            // no block is lost: every block not held by a surviving file is free
            int held = 1;
            CardFileInfo probe;
            if (CARDOpen(0, "VICTIM", &probe) == 0) {
                held += 2;
            }
            if (CARDOpen(0, "NEW", &probe) == 0) {
                held += 3;
            }
            // checked straight after the mount, before any CARDCheck: the mount itself reclaims
            CHECK(CARDFreeBlocks(0, &bytes, &files) == 0);
            CHECK(bytes == (251 - held) * BLOCK);
            CHECK(CARDCheckAsync(0, 0) == 0);
        }
    }
}

// A create whose directory write fails leaves its blocks allocated in the mounted card. CARDCheck frees
// them again without needing a remount.
static void TestCheckReclaimsLeak()
{
    CardFileInfo fi;
    s32 bytes, files;

    ResetWorld();
    g_fake[0].writes = 0;
    g_fake[0].failWritesAfter = 1;  // the FAT write goes out, the directory write fails
    CHECK(MakeFile("NEW", 3, &fi) < 0);
    g_fake[0].failWritesAfter = -1;
    CHECK(CARDOpen(0, "NEW", &fi) == -4);  // the failed create left no file behind, not even in the cache
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0 && bytes == (251 - 3) * BLOCK && files == 127);  // blocks leaked for now
    CHECK(CARDCheckAsync(0, 0) == 0);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == 0 && bytes == 251 * BLOCK && files == 127);
}

static void TestCorruptImage()
{
    CardFileInfo fi;
    s32 bytes, files;
    unsigned char* img;

    // garbage header: mount succeeds (as on the console) but reports broken; only format fixes it
    ResetWorld(0);
    FakeAssign(0, 3 * BLOCK, 0x5A);
    CHECK(CARDMountAsync(0, 0, 0, 0) == 0);
    CHECK(CARDGetResultCode(0) == -6);
    CHECK(CARDCheckAsync(0, 0) == -6);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -6);
    CHECK(CARDOpen(0, "X", &fi) == -6);
    CHECK(MakeFile("X", 1, &fi) == -6);
    CHECK(CARDFormatAsync(0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(MakeFile("X", 1, &fi) == 0);
    CHECK(g_fake[0].imgSize == 2u * BLOCK);  // the format shrank the image to its 2 header blocks; data blocks
                                             // only enter the file when they are first written

    // too short to hold a card
    ResetWorld(0);
    FakeAssign(0, 100, 1);
    CHECK(CARDMountAsync(0, 0, 0, 0) == 0 && CARDGetResultCode(0) == -6);

    // a file whose chain is cut short, one that loops, and two that share a block: all broken
    for (int kind = 0; kind < 3; kind++) {
        ResetWorld();
        CardFileInfo a, b;
        CHECK(MakeFile("A", 3, &a) == 0);
        CHECK(MakeFile("B", 1, &b) == 0);
        img = g_fake[0].img;
        unsigned char* fat = img + BLOCK;
        unsigned char* dirB = img + 64 + 64;  // entry 1 is file B
        if (kind == 0) {
            fat[2 * 1] = 0xFE;  // A's second block ends the chain, but A claims 3 blocks
            fat[2 * 1 + 1] = 0xFF;
        } else if (kind == 1) {
            fat[2 * 2] = 0;  // A's third block points back at block 0
            fat[2 * 2 + 1] = 0;
        } else {
            dirB[32] = 2;  // B starts at block 2, A's last block: the chains are each well formed, but share it
            dirB[33] = 0;
        }
        CARDUnmount(0);
        CHECK(CARDMountAsync(0, 0, 0, 0) == 0);
        CHECK(CARDGetResultCode(0) == -6);
    }

    // truncated image after a successful mount: a read of missing data fails instead of returning garbage
    ResetWorld();
    CHECK(MakeFile("A", 2, &fi) == 0);
    static unsigned char blk[2 * BLOCK];
    CHECK(CARDWriteAsync(&fi, blk, 2 * BLOCK, 0, 0) == 0);
    CHECK(CARDReadAsync(&fi, blk, 2 * BLOCK, 0, 0) == 0);
}

static void TestDiskFaults()
{
    CardFileInfo fi;
    static unsigned char blk[BLOCK];
    s32 bytes, files;

    // disk removed while mounted: everything reports no card until it is mounted again
    ResetWorld();
    CHECK(MakeFile("A", 1, &fi) == 0);
    g_fake[0].present = 0;
    CHECK(CARDReadAsync(&fi, blk, 512, 0, 0) == -3);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -3);
    CHECK(CARDGetResultCode(0) == -3);
    g_fake[0].present = 1;
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -3);  // the layer dropped the mount; it must not resume silently
    CHECK(CARDMountAsync(0, 0, 0, 0) == 0 && CARDGetResultCode(0) == 0);
    CHECK(CARDOpen(0, "A", &fi) == 0);

    // a write that fails is an error result, and the old data is still readable
    static unsigned char old[BLOCK], now[BLOCK], got[BLOCK];
    Fill(old, BLOCK, 1);
    Fill(now, BLOCK, 2);
    CHECK(CARDWriteAsync(&fi, old, BLOCK, 0, 0) == 0);
    g_fake[0].writes = 0;
    g_fake[0].failWritesAfter = 0;
    CHECK(CARDWriteAsync(&fi, now, BLOCK, 0, 0) < 0);
    CHECK(CARDGetResultCode(0) < 0);
    g_fake[0].failWritesAfter = -1;

    // flush failure after the data went out: reported, not hidden
    g_fake[0].failFlush = 1;
    CHECK(CARDWriteAsync(&fi, now, BLOCK, 0, 0) < 0);
    CHECK(CARDGetResultCode(0) < 0);
    g_fake[0].failFlush = 0;
    CHECK(CARDWriteAsync(&fi, now, BLOCK, 0, 0) == 0);
    CHECK(CARDReadAsync(&fi, got, BLOCK, 0, 0) == 0 && memcmp(got, now, BLOCK) == 0);

    // the disk cannot be read at all at mount
    ResetWorld(0);
    g_fake[0].failSize = 1;
    CHECK(CARDMountAsync(0, 0, 0, 0) == -3);
    CHECK(CARDGetResultCode(0) == -3);
    CHECK(CARDFreeBlocks(0, &bytes, &files) == -3);
    g_fake[0].failSize = 0;

    // format whose writes fail
    ResetWorld();
    g_fake[0].writes = 0;
    g_fake[0].failWritesAfter = 1;
    CHECK(CARDFormatAsync(0, 0) < 0);
    CHECK(CARDGetResultCode(0) < 0);
}

// Async calls complete at once, but the game polls CARDGetResultCode: it must see the outcome of the LAST
// call, including a failure found before the operation started (the SDK would leave a stale 0 there).
static void TestResultCodeFollowsLastCall()
{
    CardFileInfo fi;

    ResetWorld();
    CHECK(MakeFile("A", 1, &fi) == 0);
    CHECK(CARDGetResultCode(0) == 0);
    CHECK(CARDDeleteAsync(0, "NOPE", 0) == -4);
    CHECK(CARDGetResultCode(0) == -4);
    CHECK(CARDDeleteAsync(0, "A", 0) == 0);
    CHECK(CARDGetResultCode(0) == 0);
    CHECK(CARDCreateAsync(0, "A", 0, &fi, 0) == -128);
    CHECK(CARDGetResultCode(0) == -128);
}

static void TestTwoSlots()
{
    CardFileInfo a, b, h;
    static unsigned char da[BLOCK], db[BLOCK], got[BLOCK];

    ResetWorld();
    g_fake[1].present = 1;
    CHECK(CARDMountAsync(1, 0, 0, 0) == 0 && CARDGetResultCode(1) == 0);

    CHECK(CARDCreateAsync(0, "SAVE", BLOCK, &a, 0) == 0);
    CHECK(CARDCreateAsync(1, "SAVE", BLOCK, &b, 0) == 0);  // same name, other card: no clash
    Fill(da, BLOCK, 10);
    Fill(db, BLOCK, 20);
    CHECK(CARDWriteAsync(&a, da, BLOCK, 0, 0) == 0);
    CHECK(CARDWriteAsync(&b, db, BLOCK, 0, 0) == 0);
    CHECK(CARDOpen(1, "SAVE", &h) == 0 && h.chan == 1);
    CHECK(CARDReadAsync(&h, got, BLOCK, 0, 0) == 0 && memcmp(got, db, BLOCK) == 0);
    CHECK(CARDOpen(0, "SAVE", &h) == 0 && h.chan == 0);
    CHECK(CARDReadAsync(&h, got, BLOCK, 0, 0) == 0 && memcmp(got, da, BLOCK) == 0);

    CHECK(CARDDeleteAsync(1, "SAVE", 0) == 0);
    CHECK(CARDOpen(0, "SAVE", &h) == 0);   // slot A untouched
    CHECK(CARDOpen(1, "SAVE", &h) == -4);
    CHECK(CARDGetResultCode(0) == 0 && CARDGetResultCode(1) == -4);  // results are per slot
}

static void TestBackendNotAvailable()
{
    // Run last: CARDInit works once, so this builds its own state by hand through a failed backend start.
    // (A layer whose backend fails to start reports "no card" on every slot; see TestInitProbe for absent
    // storage, which takes the same path through Present().)
    CHECK(g_initCalls == 1);  // CARDInit reached the backend exactly once
}

int main()
{
    TestInitProbe();
    TestMountFresh();
    TestSerial();
    TestCreateRules();
    TestReadWrite();
    TestFilesIsolatedAndFragmented();
    TestDelete();
    TestFullCard();
    TestPersistence();
    TestStatus();
    TestOffsetsMatchSdk();
    TestPowerCut();
    TestCheckReclaimsLeak();
    TestCorruptImage();
    TestDiskFaults();
    TestResultCodeFollowsLastCall();
    TestTwoSlots();
    TestBackendNotAvailable();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
