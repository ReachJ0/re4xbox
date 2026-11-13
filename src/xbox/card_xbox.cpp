// xbox/card_xbox: Xbox replacement for the Dolphin SDK CARD library (src/lib/CARD*.c).
//
// Provides exactly the 17 CARD calls game/card.cpp makes and nothing else:
//   CARDInit CARDProbeEx CARDMountAsync CARDUnmount CARDCheckAsync CARDFormatAsync CARDFreeBlocks
//   CARDCreateAsync CARDOpen CARDClose CARDReadAsync CARDWriteAsync CARDDeleteAsync CARDGetStatus
//   CARDSetStatusAsync CARDGetSerialNo CARDGetResultCode
// Link this file INSTEAD of src/lib/CARD*.c (CARDBios, CARDBlock, CARDCheck, CARDCreate, CARDDelete,
// CARDDir, CARDFormat, CARDMount, CARDNet, CARDOpen, CARDRdwr, CARDRead, CARDStat, CARDUnlock, CARDWrite).
//
// How it works: each slot is a memory card emulated inside one image file, reached only through the
// XCardBackend_* byte-blob calls (xbox/xcard_backend.h). The image keeps the card's own structure, because
// that is the contract the game's save code is written against: 8 KB blocks, at most 127 files, a
// directory, and a block allocation table (FAT) that chains a file's blocks.
//
//   image block 0   header (magic, version, card id) + 127 directory entries of 64 bytes
//   image block 1   FAT: one u16 per data block, 0xFFFF free, 0xFFFE end of chain, else the next block
//   image block 2+  251 data blocks, the size of a Memory Card 251 (16 Mbit), so the game's own "not
//                   enough space" checks behave as they do on the console
//
// All multi-byte fields in the image are little-endian, written byte by byte, so the file does not depend
// on struct padding. Nothing here can be copied to or from a real GameCube card: the format differs, and
// the game's save data is byte-order dependent in its own right (see the endianness step of the port).
//
// Every operation finishes before it returns, so an "async" call stores its result at once and
// CARDGetResultCode returns it; the game's state machines already poll that result, so they run straight
// through. A completion callback is accepted and ignored (the game always passes 0). Unlike the SDK, an
// error found before an async call starts is also stored as the result, so a state machine that ignores
// the return value and polls the result cannot mistake a stale "success" for the outcome.
//
// Only compiled for the Xbox build (RE4_XBOX); it has no meaning on the GameCube target.
#ifdef RE4_XBOX

#include "card.h"  // game header: CardFileInfo, CardStat, CardCallback and the prototypes defined here
#include "xbox/xcard_backend.h"

#include <string.h>

// CARD_RESULT_* codes, as the game compares them
#define R_READY 0
#define R_NOCARD (-3)
#define R_NOFILE (-4)
#define R_BROKEN (-6)
#define R_EXIST (-7)
#define R_NOENT (-8)
#define R_INSSPACE (-9)
#define R_LIMIT (-11)
#define R_NAMETOOLONG (-12)
#define R_FATAL (-128)

#define BLOCK_SIZE 0x2000  // card sector size (the game checks for exactly this)
#define DATA_BLOCKS 251
#define IMG_HDR_BLOCKS 2   // directory block + FAT block before the data
#define MAX_FILES 127
#define MEM_MBIT 16        // what CARDProbeEx reports for the emulated card
#define NAME_MAX_LEN 32
#define READ_ALIGN 512     // CARD_READ_SIZE: reads are in 512-byte units, writes in whole blocks

#define FAT_FREE 0xFFFF
#define FAT_END 0xFFFE
#define NO_ICON 0xFFFFFFFFu

// header
#define H_MAGIC 0        // "RE4C"
#define H_VERSION 4
#define H_SERIAL 8
#define H_BLOCKS 12
#define H_DIR 64         // 127 * 64 = 8128 bytes, ends exactly at BLOCK_SIZE
#define ENTRY_SIZE 64
#define IMAGE_VERSION 1

// directory entry
#define E_NAME 0         // 32 bytes, NUL padded, no terminator if the name is 32 characters
#define E_START 32       // u16 first data block
#define E_LEN 34         // u16 length in blocks
#define E_TIME 36        // u32 seconds since 2000-01-01
#define E_ICONADDR 40    // u32
#define E_COMMENT 44     // u32
#define E_ICONFMT 48     // u16
#define E_ICONSPD 50     // u16
#define E_GAME 52        // 4 bytes (unused here, kept for the structure)
#define E_COMPANY 56     // 2 bytes
#define E_BANNER 58      // u8
#define E_USED 59        // u8: 1 in use

struct Slot {
    int mounted;
    int broken;        // mounted but the image is not valid: only format / unmount / probe work
    s32 result;        // last CARD result, what CARDGetResultCode reports
    u32 imgSize;       // image size as the backend last reported it
    u8 hdr[BLOCK_SIZE];  // image block 0
    u8 fat[BLOCK_SIZE];  // image block 1
};

static Slot s_slot[XCARD_SLOTS];
static int s_backendOk = 0;

static u16 Get16(const u8* p) { return (u16) (p[0] | (p[1] << 8)); }
static u32 Get32(const u8* p) { return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24); }
static void Put16(u8* p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
}
static void Put32(u8* p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
    p[2] = (u8) (v >> 16);
    p[3] = (u8) (v >> 24);
}

static inline u8* Entry(Slot* s, int fileNo) { return s->hdr + H_DIR + fileNo * ENTRY_SIZE; }
static inline int EntryUsed(Slot* s, int fileNo) { return Entry(s, fileNo)[E_USED] == 1; }
static inline u16 FatGet(Slot* s, u32 block) { return Get16(s->fat + block * 2); }
static inline void FatSet(Slot* s, u32 block, u32 v) { Put16(s->fat + block * 2, v); }
static inline u32 DataOffset(u32 block) { return (u32) (IMG_HDR_BLOCKS + block) * BLOCK_SIZE; }
static inline int ChanOk(s32 chan) { return chan >= 0 && chan < XCARD_SLOTS; }

// ---- image I/O. The directory (block 0) and FAT (block 1) are cached in the Slot and written back whole
// after a change. A reader of the data area sees zeros for blocks that were allocated but never written.

static int WriteRaw(int chan, u32 off, const void* buf, u32 len)
{
    Slot* s = &s_slot[chan];
    if (!XCardBackend_Write(chan, off, buf, len)) {
        return 0;
    }
    if (off + len > s->imgSize) {
        s->imgSize = off + len;
    }
    return 1;
}

static int ReadRaw(int chan, u32 off, void* buf, u32 len)
{
    Slot* s = &s_slot[chan];
    u32 have = 0;

    if (off < s->imgSize) {
        have = s->imgSize - off;
        if (have > len) {
            have = len;
        }
        if (!XCardBackend_Read(chan, off, buf, have)) {
            return 0;
        }
    }
    if (have < len) {
        memset((u8*) buf + have, 0, len - have);
    }
    return 1;
}

// After a failed write the cached directory / FAT hold a change that never reached the disk, and the game
// would go on seeing a file that is not there. Reload both from the disk, so the cache is what a remount
// would see (a half-done create or delete then shows up as leaked blocks, which are reclaimed). If even
// the reload fails the slot is unmounted: nothing is served from a cache that may be wrong.
static void ResyncCache(int chan)
{
    Slot* s = &s_slot[chan];
    unsigned int size;

    if (!XCardBackend_Size(chan, &size) || size < (unsigned int) IMG_HDR_BLOCKS * BLOCK_SIZE ||
        !XCardBackend_Read(chan, 0, s->hdr, BLOCK_SIZE) || !XCardBackend_Read(chan, BLOCK_SIZE, s->fat, BLOCK_SIZE)) {
        s->mounted = 0;
        return;
    }
    s->imgSize = size;
}

static int StoreHdr(int chan)
{
    if (WriteRaw(chan, 0, s_slot[chan].hdr, BLOCK_SIZE) && XCardBackend_Flush(chan)) {
        return 1;
    }
    ResyncCache(chan);
    return 0;
}

static int StoreFat(int chan)
{
    if (WriteRaw(chan, BLOCK_SIZE, s_slot[chan].fat, BLOCK_SIZE) && XCardBackend_Flush(chan)) {
        return 1;
    }
    ResyncCache(chan);
    return 0;
}

// ---- image structure

static void InitImage(int chan)
{
    Slot* s = &s_slot[chan];
    u32 i;

    memset(s->hdr, 0, BLOCK_SIZE);
    memset(s->fat, 0, BLOCK_SIZE);
    memcpy(s->hdr + H_MAGIC, "RE4C", 4);
    Put32(s->hdr + H_VERSION, IMAGE_VERSION);
    // A new card id on every format, like a real card; chan keeps the two slots distinct
    Put32(s->hdr + H_SERIAL, (XCardBackend_Seconds2000() * 2654435761u) ^ (u32) (chan + 1));
    Put32(s->hdr + H_BLOCKS, DATA_BLOCKS);
    for (i = 0; i < DATA_BLOCKS; i++) {
        FatSet(s, i, FAT_FREE);
    }
}

static int FreeBlocks(Slot* s)
{
    int n = 0;
    u32 i;
    for (i = 0; i < DATA_BLOCKS; i++) {
        if (FatGet(s, i) == FAT_FREE) {
            n++;
        }
    }
    return n;
}

static int FreeFiles(Slot* s)
{
    int n = 0;
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (!EntryUsed(s, i)) {
            n++;
        }
    }
    return n;
}

// 1 if the directory and FAT agree: every file's chain has exactly its length, stays inside the data
// area and ends properly, and no block belongs to two files. Blocks that are allocated to nobody are NOT an
// error: create and delete update the FAT and the directory in two writes, so a power cut between them
// leaves such blocks behind. ReclaimLeaks frees them.
static int ImageValid(Slot* s, u8* owner)
{
    int f;

    if (memcmp(s->hdr + H_MAGIC, "RE4C", 4) != 0 || Get32(s->hdr + H_VERSION) != IMAGE_VERSION ||
        Get32(s->hdr + H_BLOCKS) != DATA_BLOCKS) {
        return 0;
    }
    memset(owner, 0, DATA_BLOCKS);

    for (f = 0; f < MAX_FILES; f++) {
        u8* e = Entry(s, f);
        u32 b;
        u32 n;

        if (e[E_USED] == 0) {
            continue;
        }
        if (e[E_USED] != 1 || Get16(e + E_LEN) == 0) {
            return 0;
        }
        b = Get16(e + E_START);
        for (n = 0; n < Get16(e + E_LEN); n++) {
            u16 next;
            if (b >= DATA_BLOCKS || owner[b]) {
                return 0;
            }
            owner[b] = 1;
            next = FatGet(s, b);
            if (n + 1 == Get16(e + E_LEN)) {
                if (next != FAT_END) {
                    return 0;
                }
            } else {
                b = next;
            }
        }
    }
    return 1;
}

// Frees blocks the FAT marks allocated but no file owns (`owner` comes from ImageValid). Returns how many.
static int ReclaimLeaks(Slot* s, const u8* owner)
{
    int n = 0;
    u32 i;

    for (i = 0; i < DATA_BLOCKS; i++) {
        if (!owner[i] && FatGet(s, i) != FAT_FREE) {
            FatSet(s, i, FAT_FREE);
            n++;
        }
    }
    return n;
}

// Loads and validates the slot's image. A brand-new (empty) image is formatted on the spot, so the first
// run does not push the player through a "format this card?" prompt for a disk that never was a card.
// Returns R_READY, R_BROKEN for an image that is there but not valid (the game then offers to format it),
// or R_NOCARD if the backend cannot be read at all.
static s32 LoadImage(int chan)
{
    Slot* s = &s_slot[chan];
    u8 owner[DATA_BLOCKS];
    unsigned int size;  // not u32: the backend takes `unsigned int*`, and u32 is `unsigned long` in types.h

    s->broken = 0;
    if (!XCardBackend_Size(chan, &size)) {
        return R_NOCARD;
    }
    s->imgSize = size;

    if (size == 0) {
        InitImage(chan);
        if (!StoreFat(chan) || !StoreHdr(chan)) {
            return R_NOCARD;
        }
        return R_READY;
    }
    if (size < (u32) IMG_HDR_BLOCKS * BLOCK_SIZE || !XCardBackend_Read(chan, 0, s->hdr, BLOCK_SIZE) ||
        !XCardBackend_Read(chan, BLOCK_SIZE, s->fat, BLOCK_SIZE) || !ImageValid(s, owner)) {
        s->broken = 1;
        return R_BROKEN;
    }
    if (ReclaimLeaks(s, owner) > 0 && !StoreFat(chan)) {
        return R_NOCARD;
    }
    return R_READY;
}

// ---- common prologue

static s32 Done(int chan, s32 r)
{
    if (ChanOk(chan)) {
        s_slot[chan].result = r;
    }
    return r;
}

// Every operation but probe / mount / unmount starts here: valid slot, storage still there, mounted, and
// (unless `allowBroken`) a valid image. Returns R_READY or the error to hand back.
static s32 Begin(int chan, int allowBroken)
{
    if (!ChanOk(chan)) {
        return R_FATAL;
    }
    if (!s_backendOk || !XCardBackend_Present(chan)) {
        s_slot[chan].mounted = 0;
        return R_NOCARD;
    }
    if (!s_slot[chan].mounted) {
        return R_NOCARD;
    }
    if (s_slot[chan].broken && !allowBroken) {
        return R_BROKEN;
    }
    return R_READY;
}

// Maps a file name to the fixed 32-byte form the directory stores. 0 if it is too long.
static int PackName(const char* name, u8* out)
{
    size_t n = strlen(name);
    if (n > NAME_MAX_LEN) {
        return 0;
    }
    memset(out, 0, NAME_MAX_LEN);
    memcpy(out, name, n);
    return 1;
}

static int FindFile(Slot* s, const u8* packed)
{
    int f;
    for (f = 0; f < MAX_FILES; f++) {
        if (EntryUsed(s, f) && memcmp(Entry(s, f) + E_NAME, packed, NAME_MAX_LEN) == 0) {
            return f;
        }
    }
    return -1;
}

// Data block holding byte `off` of file `f`, or -1 if the chain is shorter than that.
static int BlockOf(Slot* s, int f, u32 off)
{
    u8* e = Entry(s, f);
    u32 b = Get16(e + E_START);
    u32 skip = off / BLOCK_SIZE;

    while (skip-- > 0) {
        if (b >= DATA_BLOCKS) {
            return -1;
        }
        b = FatGet(s, b);
    }
    return b < DATA_BLOCKS ? (int) b : -1;
}

// Where the banner, icons and their palettes sit inside the file, as the SDK computes them from the
// directory fields (same layout rules, same results as the card's own CARDGetStatus).
static void UpdateIconOffsets(u32 iconAddr, u32 bannerFormat, u32 iconFormat, CardStat* stat)
{
    u32 offset = iconAddr;
    int iconTlut = 0;
    int i;

    if (offset == NO_ICON) {
        // The SDK clears the caller's format fields but still lays out the offsets from the stored ones
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }

    switch (bannerFormat & 3) {
    case 1:  // C8: 96x32 indices + 256 palette entries
        stat->offsetBanner = offset;
        offset += 96 * 32;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case 2:  // RGB5A3: 96x32 x 2 bytes
        stat->offsetBanner = offset;
        offset += 2 * 96 * 32;
        stat->offsetBannerTlut = NO_ICON;
        break;
    default:
        stat->offsetBanner = NO_ICON;
        stat->offsetBannerTlut = NO_ICON;
        break;
    }

    for (i = 0; i < 8; i++) {
        switch ((iconFormat >> (2 * i)) & 3) {
        case 1:  // C8: 32x32 indices, shares one palette
            stat->offsetIcon[i] = offset;
            offset += 32 * 32;
            iconTlut = 1;
            break;
        case 2:  // RGB5A3
            stat->offsetIcon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            stat->offsetIcon[i] = NO_ICON;
            break;
        }
    }

    if (iconTlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = NO_ICON;
    }
    stat->offsetData = offset;
}

// ---- the CARD API

extern "C" {

void CARDInit()
{
    int i;

    if (s_backendOk) {
        return;  // like the SDK, a second init does nothing
    }
    memset(s_slot, 0, sizeof(s_slot));
    s_backendOk = XCardBackend_Init();
    for (i = 0; i < XCARD_SLOTS; i++) {
        s_slot[i].result = R_READY;
    }
}

s32 CARDGetResultCode(s32 chan)
{
    if (!ChanOk(chan)) {
        return R_FATAL;
    }
    return s_slot[chan].result;
}

// Reports size / sector size of the "card" in the slot without mounting it. Null out-pointers are fine.
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    if (!ChanOk(chan)) {
        return R_FATAL;
    }
    if (!s_backendOk || !XCardBackend_Present(chan)) {
        return R_NOCARD;
    }
    if (memSize) {
        *memSize = MEM_MBIT;
    }
    if (sectorSize) {
        *sectorSize = BLOCK_SIZE;
    }
    return R_READY;
}

// `workArea`, `detachCallback` and `attachCallback` are not needed: there is no card to talk to and no
// removal event. R_BROKEN (mounted, image invalid) is a success the game handles, as on the console.
s32 CARDMountAsync(s32 chan, void* workArea, CardCallback detachCallback, CardCallback attachCallback)
{
    s32 r;

    (void) workArea;
    (void) detachCallback;
    (void) attachCallback;
    if (!ChanOk(chan)) {
        return R_FATAL;
    }
    if (!s_backendOk || !XCardBackend_Present(chan)) {
        s_slot[chan].mounted = 0;
        return Done(chan, R_NOCARD);
    }
    r = LoadImage(chan);
    s_slot[chan].mounted = (r != R_NOCARD);
    Done(chan, r);
    return r == R_NOCARD ? r : R_READY;  // the call itself started fine; the outcome is in the result
}

s32 CARDUnmount(s32 chan)
{
    if (!ChanOk(chan)) {
        return R_FATAL;
    }
    s_slot[chan].mounted = 0;
    s_slot[chan].broken = 0;
    return Done(chan, R_READY);
}

// Checks the image's directory against its FAT.
s32 CARDCheckAsync(s32 chan, CardCallback callback)
{
    u8 owner[DATA_BLOCKS];
    s32 r = Begin(chan, 1);

    (void) callback;
    if (r < 0) {
        return Done(chan, r);
    }
    if (s_slot[chan].broken || !ImageValid(&s_slot[chan], owner)) {
        s_slot[chan].broken = 1;
        return Done(chan, R_BROKEN);
    }
    if (ReclaimLeaks(&s_slot[chan], owner) > 0 && !StoreFat(chan)) {
        return Done(chan, R_NOCARD);
    }
    Done(chan, R_READY);
    return R_READY;
}

// Erases the card: empty directory, empty FAT, new card id. Works on a broken image, which is the point.
s32 CARDFormatAsync(s32 chan, CardCallback callback)
{
    s32 r = Begin(chan, 1);

    (void) callback;
    if (r < 0) {
        return Done(chan, r);
    }
    InitImage(chan);
    if (!StoreFat(chan) || !StoreHdr(chan) || !XCardBackend_Truncate(chan, IMG_HDR_BLOCKS * BLOCK_SIZE)) {
        return Done(chan, R_NOCARD);
    }
    s_slot[chan].imgSize = IMG_HDR_BLOCKS * BLOCK_SIZE;
    s_slot[chan].broken = 0;
    Done(chan, R_READY);
    return R_READY;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    s32 r = Begin(chan, 0);

    if (r < 0) {
        return Done(chan, r);
    }
    if (byteNotUsed) {
        *byteNotUsed = FreeBlocks(&s_slot[chan]) * BLOCK_SIZE;
    }
    if (filesNotUsed) {
        *filesNotUsed = FreeFiles(&s_slot[chan]);
    }
    return Done(chan, R_READY);
}

// Creates `fileName` of `size` bytes (a multiple of 8 KB) and leaves it open in *fileInfo, as the SDK does.
s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CardFileInfo* fileInfo, CardCallback callback)
{
    Slot* s;
    u8 name[NAME_MAX_LEN];
    u32 blocks;
    u32 chain[DATA_BLOCKS];
    u32 n;
    u32 i;
    int freeNo = -1;
    s32 r;

    (void) callback;
    if (!ChanOk(chan) || !fileName || !fileInfo) {
        return Done(chan, R_FATAL);
    }
    if (!PackName(fileName, name)) {
        return Done(chan, R_NAMETOOLONG);
    }
    r = Begin(chan, 0);
    if (r < 0) {
        return Done(chan, r);
    }
    s = &s_slot[chan];
    if (size == 0 || (size % BLOCK_SIZE) != 0) {
        return Done(chan, R_FATAL);
    }
    blocks = size / BLOCK_SIZE;

    if (FindFile(s, name) >= 0) {
        return Done(chan, R_EXIST);
    }
    for (i = 0; i < MAX_FILES; i++) {
        if (!EntryUsed(s, (int) i)) {
            freeNo = (int) i;
            break;
        }
    }
    if (freeNo < 0) {
        return Done(chan, R_NOENT);
    }
    if (blocks > (u32) FreeBlocks(s)) {
        return Done(chan, R_INSSPACE);
    }

    // take the first `blocks` free blocks and chain them
    n = 0;
    for (i = 0; i < DATA_BLOCKS && n < blocks; i++) {
        if (FatGet(s, i) == FAT_FREE) {
            chain[n++] = i;
        }
    }
    for (i = 0; i + 1 < n; i++) {
        FatSet(s, chain[i], chain[i + 1]);
    }
    FatSet(s, chain[n - 1], FAT_END);
    if (!StoreFat(chan)) {
        return Done(chan, R_NOCARD);
    }

    {
        u8* e = Entry(s, freeNo);
        memset(e, 0, ENTRY_SIZE);
        memcpy(e + E_NAME, name, NAME_MAX_LEN);
        Put16(e + E_START, chain[0]);
        Put16(e + E_LEN, blocks);
        Put32(e + E_TIME, XCardBackend_Seconds2000());
        Put32(e + E_ICONADDR, NO_ICON);
        Put32(e + E_COMMENT, NO_ICON);
        e[E_USED] = 1;
    }
    if (!StoreHdr(chan)) {
        return Done(chan, R_NOCARD);
    }

    fileInfo->chan = chan;
    fileInfo->fileNo = freeNo;
    fileInfo->offset = 0;
    fileInfo->length = -1;
    fileInfo->iBlock = (u16) chain[0];
    Done(chan, R_READY);
    return R_READY;
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, CardCallback callback)
{
    Slot* s;
    u8 name[NAME_MAX_LEN];
    int f;
    u32 b;
    s32 r;

    (void) callback;
    if (!ChanOk(chan) || !fileName) {
        return Done(chan, R_FATAL);
    }
    if (!PackName(fileName, name)) {
        return Done(chan, R_NAMETOOLONG);
    }
    r = Begin(chan, 0);
    if (r < 0) {
        return Done(chan, r);
    }
    s = &s_slot[chan];
    f = FindFile(s, name);
    if (f < 0) {
        return Done(chan, R_NOFILE);
    }

    // Directory entry first, then the FAT. A power cut in between only leaks the blocks, which the next
    // mount reclaims; the other order could leave a file pointing at blocks that are free again.
    b = Get16(Entry(s, f) + E_START);
    u32 n = Get16(Entry(s, f) + E_LEN);
    memset(Entry(s, f), 0, ENTRY_SIZE);
    if (!StoreHdr(chan)) {
        return Done(chan, R_NOCARD);
    }
    for (; n > 0 && b < DATA_BLOCKS; n--) {
        u32 next = FatGet(s, b);
        FatSet(s, b, FAT_FREE);
        b = next;
    }
    if (!StoreFat(chan)) {
        return Done(chan, R_NOCARD);
    }
    Done(chan, R_READY);
    return R_READY;
}

s32 CARDOpen(s32 chan, const char* fileName, CardFileInfo* fileInfo)
{
    u8 name[NAME_MAX_LEN];
    int f;
    s32 r;

    if (!fileInfo) {
        return Done(chan, R_FATAL);
    }
    fileInfo->chan = -1;
    if (!ChanOk(chan) || !fileName) {
        return Done(chan, R_FATAL);
    }
    if (!PackName(fileName, name)) {
        return Done(chan, R_NAMETOOLONG);
    }
    r = Begin(chan, 0);
    if (r < 0) {
        return Done(chan, r);
    }
    f = FindFile(&s_slot[chan], name);
    if (f < 0) {
        return Done(chan, R_NOFILE);
    }
    fileInfo->chan = chan;
    fileInfo->fileNo = f;
    fileInfo->offset = 0;
    fileInfo->length = -1;
    fileInfo->iBlock = Get16(Entry(&s_slot[chan], f) + E_START);
    return Done(chan, R_READY);
}

s32 CARDClose(CardFileInfo* fileInfo)
{
    s32 chan;

    if (!fileInfo || !ChanOk(fileInfo->chan)) {
        return R_FATAL;
    }
    chan = fileInfo->chan;
    fileInfo->chan = -1;
    return Done(chan, R_READY);
}

// Shared by read and write: checks the open file, then the alignment and range rules of the SDK.
static s32 FileOp(CardFileInfo* fi, s32 length, s32 offset, u32 align, int* chanOut, int* fileOut)
{
    Slot* s;
    s32 r;
    u32 fileLen;

    if (!fi || !ChanOk(fi->chan)) {
        return R_FATAL;
    }
    r = Begin(fi->chan, 0);
    if (r < 0) {
        return r;
    }
    s = &s_slot[fi->chan];
    if (fi->fileNo < 0 || fi->fileNo >= MAX_FILES || !EntryUsed(s, fi->fileNo)) {
        return R_NOFILE;
    }
    if (length <= 0 || offset < 0 || ((u32) length % align) != 0 || ((u32) offset % align) != 0) {
        return R_FATAL;
    }
    fileLen = (u32) Get16(Entry(s, fi->fileNo) + E_LEN) * BLOCK_SIZE;
    if ((u32) offset >= fileLen || (u32) length > fileLen - (u32) offset) {
        return R_LIMIT;
    }
    *chanOut = fi->chan;
    *fileOut = fi->fileNo;
    return R_READY;
}

s32 CARDReadAsync(CardFileInfo* fi, void* buf, s32 length, s32 offset, CardCallback callback)
{
    int chan = 0;
    int f = 0;
    s32 r;
    u32 done = 0;

    (void) callback;
    r = FileOp(fi, length, offset, READ_ALIGN, &chan, &f);
    if (r < 0) {
        return Done(fi ? fi->chan : -1, r);
    }
    if (!buf) {
        return Done(chan, R_FATAL);
    }
    while (done < (u32) length) {
        u32 pos = (u32) offset + done;
        u32 inBlock = pos % BLOCK_SIZE;
        u32 chunk = BLOCK_SIZE - inBlock;
        int b = BlockOf(&s_slot[chan], f, pos);

        if (chunk > (u32) length - done) {
            chunk = (u32) length - done;
        }
        if (b < 0 || !ReadRaw(chan, DataOffset((u32) b) + inBlock, (u8*) buf + done, chunk)) {
            return Done(chan, R_BROKEN);
        }
        done += chunk;
    }
    Done(chan, R_READY);
    return R_READY;
}

// Writes whole blocks, stamps the file's date, and makes the result durable before reporting success.
s32 CARDWriteAsync(CardFileInfo* fi, void* buf, s32 length, s32 offset, CardCallback callback)
{
    int chan = 0;
    int f = 0;
    s32 r;
    u32 done = 0;

    (void) callback;
    r = FileOp(fi, length, offset, BLOCK_SIZE, &chan, &f);
    if (r < 0) {
        return Done(fi ? fi->chan : -1, r);
    }
    if (!buf) {
        return Done(chan, R_FATAL);
    }
    while (done < (u32) length) {
        int b = BlockOf(&s_slot[chan], f, (u32) offset + done);

        if (b < 0 || !WriteRaw(chan, DataOffset((u32) b), (u8*) buf + done, BLOCK_SIZE)) {
            return Done(chan, R_BROKEN);
        }
        done += BLOCK_SIZE;
    }
    Put32(Entry(&s_slot[chan], f) + E_TIME, XCardBackend_Seconds2000());
    if (!StoreHdr(chan)) {  // StoreHdr also flushes the data written above
        return Done(chan, R_NOCARD);
    }
    Done(chan, R_READY);
    return R_READY;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CardStat* stat)
{
    Slot* s;
    u8* e;
    s32 r;

    if (!ChanOk(chan) || fileNo < 0 || fileNo >= MAX_FILES || !stat) {
        return Done(chan, R_FATAL);
    }
    r = Begin(chan, 0);
    if (r < 0) {
        return Done(chan, r);
    }
    s = &s_slot[chan];
    if (!EntryUsed(s, fileNo)) {
        return Done(chan, R_NOFILE);
    }
    e = Entry(s, fileNo);

    memcpy(stat->gameName, e + E_GAME, 4);
    memcpy(stat->company, e + E_COMPANY, 2);
    stat->length = (u32) Get16(e + E_LEN) * BLOCK_SIZE;
    memcpy(stat->fileName, e + E_NAME, NAME_MAX_LEN);
    stat->time = Get32(e + E_TIME);
    stat->bannerFormat = e[E_BANNER];
    stat->iconAddr = Get32(e + E_ICONADDR);
    stat->iconFormat = Get16(e + E_ICONFMT);
    stat->iconSpeed = Get16(e + E_ICONSPD);
    stat->commentAddr = Get32(e + E_COMMENT);
    UpdateIconOffsets(stat->iconAddr, stat->bannerFormat, stat->iconFormat, stat);
    return Done(chan, R_READY);
}

// Stores the banner / icon / comment description and stamps the date. Same argument rules as the SDK: the
// icon data starts in the first 512 bytes, a comment must not cross an 8 KB boundary.
s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CardStat* stat, CardCallback callback)
{
    Slot* s;
    u8* e;
    s32 r;

    (void) callback;
    if (!ChanOk(chan) || fileNo < 0 || fileNo >= MAX_FILES || !stat ||
        (stat->iconAddr != NO_ICON && stat->iconAddr >= READ_ALIGN) ||
        (stat->commentAddr != NO_ICON && (stat->commentAddr % BLOCK_SIZE) > BLOCK_SIZE - 64)) {
        return Done(chan, R_FATAL);
    }
    r = Begin(chan, 0);
    if (r < 0) {
        return Done(chan, r);
    }
    s = &s_slot[chan];
    if (!EntryUsed(s, fileNo)) {
        return Done(chan, R_NOFILE);
    }
    e = Entry(s, fileNo);

    e[E_BANNER] = stat->bannerFormat;
    Put32(e + E_ICONADDR, stat->iconAddr);
    Put16(e + E_ICONFMT, stat->iconFormat);
    Put16(e + E_ICONSPD, stat->iconSpeed);
    Put32(e + E_COMMENT, stat->commentAddr);
    UpdateIconOffsets(stat->iconAddr, stat->bannerFormat, stat->iconFormat, stat);  // fills the offsets back
    if (stat->iconAddr == NO_ICON) {
        // no icon: the first icon's speed is forced to "fast", as the SDK does
        Put16(e + E_ICONSPD, (Get16(e + E_ICONSPD) & ~3u) | 1);
    }
    Put32(e + E_TIME, XCardBackend_Seconds2000());
    if (!StoreHdr(chan)) {
        return Done(chan, R_NOCARD);
    }
    Done(chan, R_READY);
    return R_READY;
}

// A stable per-card id (the game compares it between steps to notice a swapped card). It changes on format.
s32 CARDGetSerialNo(s32 chan, u64* serialNo)
{
    s32 r = Begin(chan, 1);

    if (r < 0) {
        return Done(chan, r);
    }
    if (!serialNo) {
        return Done(chan, R_FATAL);
    }
    *serialNo = ((u64) Get32(s_slot[chan].hdr + H_SERIAL) << 32) | (u32) (chan + 1);
    return Done(chan, R_READY);
}

}  // extern "C"

#endif  // RE4_XBOX
