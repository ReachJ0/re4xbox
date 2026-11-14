// Host unit test for the Xbox disc layer (src/xbox/dvd_xbox.cpp + ar_xbox.cpp). Fake in-memory backend with
// fault injection; the SDK's own DVDCompareDiskID / DVDGenerateDiskID (orig_*) are the reference for the
// disc id functions. Build and run with tools/xbox/run_dvd_test.sh.
#include <dolphin/ar.h>
#include <dolphin/dvd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "xbox/xdvd.h"
#include "xbox/xdvd_backend.h"

extern "C" int orig_DVDCompareDiskID(const DVDDiskID*, const DVDDiskID*);
extern "C" DVDDiskID* orig_DVDGenerateDiskID(DVDDiskID*, const char*, const char*, u8, u8);

static int g_checks = 0, g_fail = 0;
#define CHECK(c) \
    do { \
        g_checks++; \
        if (!(c)) { \
            g_fail++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        } \
    } while (0)

// ---- fake backend ---------------------------------------------------------------------------------------
struct FFile { const char* name; unsigned char* data; unsigned int size; int dir; };
static FFile g_files[16];
static int g_nfiles;
static int g_anyN = 0;
static int g_initOk = 1, g_failReads = 0, g_reads = 0, g_stats = 0;

static void AddFile(const char* name, unsigned int size, int dir = 0)
{
    FFile& f = g_files[g_nfiles++];
    f.name = name; f.size = size; f.dir = dir;
    f.data = (unsigned char*) malloc(size ? size : 1);
    for (unsigned i = 0; i < size; i++) f.data[i] = (unsigned char) ((i * 7 + g_nfiles * 13) & 0xFF);
}
static void ClearFiles() { for (int i = 0; i < g_nfiles; i++) free(g_files[i].data); g_nfiles = 0; }
static int Ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; }
    return *a == *b;
}
extern "C" {
int XDvdBackend_Init(void) { return g_initOk; }
int XDvdBackend_Stat(const char* path, unsigned int* size, int* is_dir)
{
    g_stats++;
    for (int i = 0; i < g_nfiles; i++) if (Ieq(g_files[i].name, path)) { *size = g_files[i].size; *is_dir = g_files[i].dir; return 1; }
    if (g_anyN && path[0] == 'n') { *size = 32; *is_dir = 0; return 1; }
    return 0;
}
int XDvdBackend_Read(const char* path, unsigned int off, void* buf, unsigned int len, unsigned int* got)
{
    g_reads++;
    if (g_failReads) return 0;
    for (int i = 0; i < g_nfiles; i++) if (Ieq(g_files[i].name, path)) {
        unsigned n = off >= g_files[i].size ? 0 : (g_files[i].size - off < len ? g_files[i].size - off : len);
        memcpy(buf, g_files[i].data + off, n); *got = n; return 1;
    }
    return 0;
}
void XDvdBackend_SetRoot(const char*) {}
}

// 32 byte aligned buffers that live below 4 GB are not needed: u32 is 64-bit in the SDK headers on this host
alignas(32) static unsigned char g_buf[3][0x2000];

static void Reset()
{
    XDvd_Reset(); ClearFiles(); g_initOk = 1; g_failReads = 0; g_reads = 0; g_stats = 0; g_anyN = 0;
    memset(g_buf, 0xEE, sizeof(g_buf));
}

// ---- callbacks -------------------------------------------------------------------------------------------
static int g_log[64], g_nlog;
static void Log(int v) { if (g_nlog < 64) g_log[g_nlog++] = v; }
static s32 g_cbResult[8]; static int g_cbCount; static DVDFileInfo* g_cbFi[8];
static void ReadCb(s32 r, DVDFileInfo* fi) { if (g_cbCount < 8) { g_cbResult[g_cbCount] = r; g_cbFi[g_cbCount] = fi; } g_cbCount++; Log(100 + (int) r % 50); }
static void ResetCb() { g_cbCount = 0; g_nlog = 0; }

// ---- tests -----------------------------------------------------------------------------------------------
static void TestPaths()
{
    Reset();
    AddFile("bio4", 0, 1); AddFile("bio4/Data.bin", 100); AddFile("a/b.dat", 64);
    AddFile("../secret", 8); AddFile("a/../b", 8); AddFile("..", 8); AddFile("a/..", 8);   // present in the fake: the layer must still refuse them
    CHECK(DVDConvertPathToEntrynum("/") == 0);
    CHECK(DVDConvertPathToEntrynum("") == 0);
    s32 e = DVDConvertPathToEntrynum("bio4/data.bin");
    CHECK(e == 1);
    CHECK(DVDConvertPathToEntrynum("/BIO4\\DATA.BIN") == e);       // case, slash, backslash
    CHECK(DVDConvertPathToEntrynum("./bio4//data.bin") == e);      // dot and double slash
    CHECK(DVDConvertPathToEntrynum("bio4/data.bin/") == e);        // trailing slash
    CHECK(DVDConvertPathToEntrynum("bio4/nope.bin") == -1);
    CHECK(DVDConvertPathToEntrynum("bio4/../bio4/data.bin") == -1); // no climbing
    CHECK(DVDConvertPathToEntrynum("..") == -1 && DVDConvertPathToEntrynum("../secret") == -1);
    CHECK(DVDConvertPathToEntrynum("a/../b") == -1 && DVDConvertPathToEntrynum("a/..") == -1 && DVDConvertPathToEntrynum("a\\..\\b") == -1);
    CHECK(DVDConvertPathToEntrynum(".hidden") == -1);                // a name starting with a dot is just missing
    CHECK(DVDConvertPathToEntrynum(NULL) == -1);
    char longp[200]; memset(longp, 'a', 199); longp[199] = 0;
    CHECK(DVDConvertPathToEntrynum(longp) == -1);
    s32 d = DVDConvertPathToEntrynum("bio4");
    CHECK(d > 0 && d != e);
    DVDFileInfo fi; memset(&fi, 0, sizeof fi);
    CHECK(DVDFastOpen(d, &fi) == FALSE);                            // a directory is not a file
    CHECK(DVDFastOpen(-1, &fi) == FALSE);
    CHECK(DVDFastOpen(999, &fi) == FALSE);
    CHECK(DVDFastOpen(0, &fi) == FALSE);                            // root
    CHECK(DVDFastOpen(e, &fi) == TRUE);
    CHECK(fi.length == 100 && fi.cb.state == DVD_STATE_END && fi.callback == NULL);
    CHECK(DVDOpen("a/b.dat", &fi) == TRUE && fi.length == 64);
    CHECK(DVDOpen("zzz", &fi) == FALSE);
    int stats = g_stats; DVDConvertPathToEntrynum("a/b.dat");
    CHECK(g_stats == stats);                                        // known entries are not looked up again
    // backend with no disc
    Reset(); g_initOk = 0; AddFile("x", 4);
    CHECK(DVDConvertPathToEntrynum("x") == -1);
    CHECK(DVDConvertPathToEntrynum("/") == 0);
}

// the entry table holds 4096 paths; the fake answers for any name that starts with 'n' + digits

static void TestTableFull()
{
    Reset(); g_anyN = 1;
    char p[16]; int last = 0, firstFail = -1;
    for (int i = 0; i < 4200; i++) {
        sprintf(p, "n%d", i);
        s32 e = DVDConvertPathToEntrynum(p);
        if (e < 0 && firstFail < 0) firstFail = i;
        if (e >= 0) last = e;
    }
    CHECK(last == 4095 && firstFail == 4095 && XDvd_ErrorCount() == 105);   // entry 0 is the root: 4095 files fit
    CHECK(DVDConvertPathToEntrynum("n7") == 8);                              // earlier entries still resolve
}

static void TestRead()
{
    Reset();
    AddFile("f1.bin", 100); AddFile("f2.bin", 8192);
    DVDFileInfo a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    CHECK(DVDOpen("f1.bin", &a) && DVDOpen("f2.bin", &b));
    ResetCb();
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 64, 0, ReadCb, 2) == TRUE);
    CHECK(g_cbCount == 0 && g_reads == 0);                           // nothing happens inside the call
    CHECK(a.cb.state == DVD_STATE_BUSY && DVDGetCommandBlockStatus(&a.cb) == DVD_STATE_BUSY);
    CHECK(DVDGetTransferredSize(&a) == 0);
    CHECK(XDvd_Pending());
    XDvd_Pump();
    CHECK(g_cbCount == 1 && g_cbResult[0] == 64 && g_cbFi[0] == &a);
    CHECK(a.cb.state == DVD_STATE_END && DVDGetTransferredSize(&a) == 64);
    CHECK(memcmp(g_buf[0], g_files[0].data, 64) == 0);
    CHECK(!XDvd_Pending() && DVDGetDriveStatus() == DVD_STATE_END);
    // past the end: file is 100 bytes, read 64 at offset 64 -> 36 real bytes then zeros
    ResetCb(); memset(g_buf[0], 0xEE, 64);
    DVDReadAsyncPrio(&a, g_buf[0], 64, 64, ReadCb, 2); XDvd_Pump();
    CHECK(g_cbResult[0] == 64 && memcmp(g_buf[0], g_files[0].data + 64, 36) == 0);
    int zeros = 1; for (int i = 36; i < 64; i++) if (g_buf[0][i]) zeros = 0; CHECK(zeros);
    // offset == length is allowed (all zeros), offset+length < length+32
    ResetCb(); DVDReadAsyncPrio(&a, g_buf[0], 0, 100, ReadCb, 2); XDvd_Pump(); CHECK(g_cbCount == 1 && g_cbResult[0] == 0);
    // refused calls
    unsigned errs = XDvd_ErrorCount();
    CHECK(DVDReadAsyncPrio(&a, g_buf[0] + 4, 32, 0, ReadCb, 2) == FALSE);  // address not 32 aligned
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 33, 0, ReadCb, 2) == FALSE);      // length
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 32, 2, ReadCb, 2) == FALSE);      // offset not multiple of 4
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 32, -4, ReadCb, 2) == FALSE);
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 32, 104, ReadCb, 2) == FALSE);    // offset > length
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 64, 96, ReadCb, 2) == FALSE);     // offset+length >= length+32
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 32, 96, ReadCb, 2) == TRUE);      // offset+length = length+28 is fine
    XDvd_Pump();
    CHECK(XDvd_ErrorCount() == errs + 6);
    DVDFileInfo raw; memset(&raw, 0, sizeof raw); raw.length = 100;
    CHECK(DVDReadAsyncPrio(&raw, g_buf[0], 32, 0, ReadCb, 2) == FALSE);    // never opened
    // a block that is still queued cannot be queued again
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2);
    CHECK(DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2) == FALSE);
    XDvd_Pump();

    // priority: the first command runs first (it is executing); then lower number first, FIFO inside
    ResetCb(); DVDFileInfo c, d; memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);
    DVDOpen("f2.bin", &c); DVDOpen("f2.bin", &d);
    DVDFileInfo e1; memset(&e1, 0, sizeof e1); DVDOpen("f1.bin", &e1);
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2);   // executing
    DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);   // waiting prio 2
    DVDReadAsyncPrio(&c, g_buf[2], 32, 0, ReadCb, 0);   // waiting prio 0 -> before b
    DVDReadAsyncPrio(&d, g_buf[2] + 64, 32, 0, ReadCb, 2); // after b
    CHECK(a.cb.state == DVD_STATE_BUSY && b.cb.state == DVD_STATE_WAITING && c.cb.state == DVD_STATE_WAITING);
    CHECK(DVDGetDriveStatus() == DVD_STATE_END);   // polling the drive pumps
    CHECK(g_cbCount == 4 && g_cbFi[0] == &a && g_cbFi[1] == &c && g_cbFi[2] == &b && g_cbFi[3] == &d);

    // large read of a big file, exact data
    ResetCb(); DVDReadAsyncPrio(&b, g_buf[0], 8192, 0, ReadCb, 2); XDvd_Pump();
    CHECK(memcmp(g_buf[0], g_files[1].data, 8192) == 0);
}

// the game's own pattern: set a "busy" flag right after the call, clear it in the callback
static int g_busy;
static void BusyCb(s32, DVDFileInfo*) { g_busy = 0; }
static DVDFileInfo* g_chainFi; static int g_chainLeft; static DVDFileInfo* g_extraFi; static int g_midState = -99, g_midDrive = -99;
static void ChainCb(s32 r, DVDFileInfo* fi)
{
    Log((int) r);
    g_midState = fi->cb.state;
    if (g_chainLeft-- > 0) DVDReadAsyncPrio(g_chainFi, g_buf[0], 32, 0, ChainCb, 2);
}
static void PostLowCb(s32, DVDFileInfo*)
{
    Log(1);
    DVDReadAsyncPrio(g_extraFi, g_buf[2], 32, 0, ReadCb, 3);   // posted from a callback, low priority
}

static DVDFileInfo* g_extraFi2;
static void PostTwoCb(s32, DVDFileInfo*)
{
    DVDReadAsyncPrio(g_extraFi, g_buf[2], 32, 0, ReadCb, 3);
    DVDReadAsyncPrio(g_extraFi2, g_buf[1], 32, 0, ReadCb, 0);
}

static void TestCallbacksOrdering()
{
    Reset(); AddFile("f.bin", 4096);
    DVDFileInfo a, b, c; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    DVDOpen("f.bin", &a); DVDOpen("f.bin", &b); DVDOpen("f.bin", &c);
    g_busy = 0;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, BusyCb, 0); g_busy = 1;      // the snd_str2 pattern
    XDvd_Pump();
    CHECK(g_busy == 0);
    // chain: the callback posts again; all complete over pumps, limit respected
    ResetCb(); g_chainFi = &a; g_chainLeft = 1000;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ChainCb, 2);
    XDvd_Pump();
    CHECK(g_nlog == 64);
    CHECK(g_chainLeft > 1000 - 300 && g_chainLeft < 1000 - 200);       // PUMP_LIMIT (256) commands per pump
    CHECK(g_midState == DVD_STATE_END);                                 // inside its own callback a block is already finished
    g_chainLeft = 0; XDvd_Pump(); CHECK(!XDvd_Pending());
    // a prio 3 command posted from a callback does not jump ahead of waiting prio 2
    ResetCb(); g_extraFi = &c;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, PostLowCb, 2);
    DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);
    XDvd_Pump();
    CHECK(g_nlog == 3 && g_log[0] == 1);                              // PostLowCb, then b (waiting), then c
    CHECK(g_cbFi[0] == &b && g_cbFi[1] == &c);
    // two commands posted from one callback on an empty queue: the better priority goes first, not the first posted
    ResetCb(); g_extraFi = &b; g_extraFi2 = &c;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, PostTwoCb, 2); XDvd_Pump();
    CHECK(g_cbCount == 2 && g_cbFi[0] == &c && g_cbFi[1] == &b);
    // prio clamp
    ResetCb(); DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 99); DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, -5); XDvd_Pump();
    CHECK(g_cbCount == 2);
}

static DVDCommandBlock* g_cancelBlk; static int g_cancelCbCalls; static s32 g_cancelRes[4];
static void CancelCb(s32 r, DVDCommandBlock*) { if (g_cancelCbCalls < 4) g_cancelRes[g_cancelCbCalls] = r; g_cancelCbCalls++; Log(200); }
static void CancelAllInCb(s32, DVDFileInfo*) { Log(300); DVDCancelAll(); }

static void TestCancel()
{
    Reset(); AddFile("f.bin", 4096);
    DVDFileInfo a, b, c; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    DVDOpen("f.bin", &a); DVDOpen("f.bin", &b); DVDOpen("f.bin", &c);
    // idle block: the callback is called at once with 0
    ResetCb(); g_cancelCbCalls = 0;
    CHECK(DVDCancelAsync(&a.cb, CancelCb) == TRUE && g_cancelCbCalls == 1 && g_cancelRes[0] == 0);
    CHECK(DVDCancelAsync(&a.cb, NULL) == TRUE);
    // waiting: dequeued, read callback gets -3, cancel callback 0, both before the call returns
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2);   // executing
    DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);   // waiting
    g_cancelCbCalls = 0;
    CHECK(DVDCancelAsync(&b.cb, CancelCb) == TRUE);
    CHECK(g_cbCount == 1 && g_cbResult[0] == -3 && g_cbFi[0] == &b && g_cancelCbCalls == 1 && g_cancelRes[0] == 0);
    CHECK(b.cb.state == DVD_STATE_CANCELED);
    XDvd_Pump();
    CHECK(g_cbCount == 2 && g_cbFi[1] == &a && g_cbResult[1] == 32);   // only the executing one is read
    CHECK(g_buf[1][0] == 0xEE);                                       // b's buffer untouched
    CHECK(DVDGetCommandBlockStatus(&b.cb) == DVD_STATE_CANCELED);
    // executing: the cancel completes with the next pump, read callback gets -6 and no data
    ResetCb(); g_cancelCbCalls = 0; memset(g_buf[0], 0xEE, 64);
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2);
    DVDReadAsyncPrio(&c, g_buf[2], 32, 0, ReadCb, 2);
    CHECK(DVDCancelAsync(&a.cb, CancelCb) == TRUE);
    CHECK(g_cbCount == 0 && g_cancelCbCalls == 0);
    CHECK(DVDCancelAsync(&c.cb, CancelCb) == TRUE);            // c is waiting: fine even while a cancel is pending
    CHECK(g_cbCount == 1 && g_cbResult[0] == -3);
    ResetCb(); g_cancelCbCalls = 0;
    XDvd_Pump();
    CHECK(g_cbCount == 1 && g_cbResult[0] == DVD_RESULT_CANCELED && g_cbFi[0] == &a && g_cancelCbCalls == 1);
    CHECK(a.cb.state == DVD_STATE_CANCELED && g_buf[0][0] == 0xEE);
    // a second cancel of the executing command while one is pending is refused
    ResetCb(); DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2);
    CHECK(DVDCancelAsync(&a.cb, NULL) == TRUE && DVDCancelAsync(&a.cb, NULL) == FALSE);
    XDvd_Pump();
    // the game's pattern: cancel with a NULL callback, command finishes with -6, drive idle after
    CHECK(DVDGetDriveStatus() == DVD_STATE_END);
    // DVDCancelAll: executing gets -6, waiting get -3, nothing left; later reads still work
    ResetCb();
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2); DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2); DVDReadAsyncPrio(&c, g_buf[2], 32, 0, ReadCb, 0);
    CHECK(DVDCancelAll() == 0);
    CHECK(g_cbCount == 3 && g_cbResult[0] == -6 && g_cbFi[0] == &a && g_cbResult[1] == -3 && g_cbResult[2] == -3);
    CHECK(!XDvd_Pending());
    ResetCb(); DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2); XDvd_Pump(); CHECK(g_cbCount == 1 && g_cbResult[0] == 32);
    // DVDCancelAll from inside a callback (re-entrancy): terminates
    ResetCb();
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, (DVDCallback) CancelAllInCb, 2); DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);
    XDvd_Pump();
    CHECK(g_cbCount == 1 && g_cbResult[0] == -3 && !XDvd_Pending());
    // DVDClose cancels what is queued, with no pump needed
    ResetCb(); DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2); DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);
    CHECK(DVDClose(&b) == TRUE); CHECK(g_cbCount == 1 && g_cbResult[0] == -3 && b.cb.state == DVD_STATE_CANCELED);
    CHECK(DVDClose(&a) == TRUE); CHECK(g_cbCount == 2 && g_cbResult[1] == -6);
    CHECK(!XDvd_Pending());
    DVDFileInfo idle; memset(&idle, 0, sizeof idle); DVDOpen("f.bin", &idle); CHECK(DVDClose(&idle) == TRUE);
    CHECK(DVDClose(NULL) == TRUE);
}

static void TestFatal()
{
    Reset(); AddFile("f.bin", 4096);
    DVDFileInfo a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    DVDOpen("f.bin", &a); DVDOpen("f.bin", &b);
    ResetCb(); g_failReads = 1;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2); DVDReadAsyncPrio(&b, g_buf[1], 32, 0, ReadCb, 2);
    CHECK(DVDGetDriveStatus() == DVD_STATE_FATAL_ERROR);
    CHECK(g_cbCount == 2 && g_cbResult[0] == -1 && g_cbResult[1] == -1);
    CHECK(a.cb.state == DVD_STATE_FATAL_ERROR && DVDGetCommandBlockStatus(&a.cb) == -1);
    int reads = g_reads; g_failReads = 0; ResetCb();
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, ReadCb, 2); XDvd_Pump();       // sticky: still fatal, no read tried
    CHECK(g_cbCount == 1 && g_cbResult[0] == -1 && g_reads == reads && DVDGetDriveStatus() == -1);
    CHECK(DVDCancelAsync(&a.cb, CancelCb) == TRUE);
    DVDCommandBlock cc; memset(&cc, 0, sizeof cc); cc.state = DVD_STATE_COVER_CLOSED;
    CHECK(DVDGetCommandBlockStatus(&cc) == 1);                          // the SDK reports 3 as 1
    cc.state = DVD_STATE_WAITING; CHECK(DVDGetCommandBlockStatus(&cc) == DVD_STATE_WAITING);
    // short read from the backend (file shrank) is also fatal
    Reset(); AddFile("g.bin", 64); DVDFileInfo s; memset(&s, 0, sizeof s); DVDOpen("g.bin", &s);
    g_files[0].size = 32;  // backend now has less than the layer saw at lookup
    ResetCb(); DVDReadAsyncPrio(&s, g_buf[0], 64, 0, ReadCb, 2); XDvd_Pump();
    CHECK(g_cbResult[0] == -1 && DVDGetDriveStatus() == -1);
}

static int g_changeDone; static void ChangeCb(s32 r, DVDCommandBlock*) { g_changeDone = 1 + (int) r; }
static void TestDiskId()
{
    Reset();
    DVDDiskID* cur = DVDGetCurrentDiskID();
    CHECK(cur != NULL && memcmp(cur->gameName, "G4BE", 4) == 0 && memcmp(cur->company, "08", 2) == 0);
    CHECK(DVDGetCurrentDiskID() == cur);
    // differential against the SDK functions over a grid
    const char* games[] = {"G4BE", "G4BJ", "G4BP", "", NULL};
    const char* comps[] = {"08", "01", "XX"};
    unsigned char nums[] = {0, 1, 0x10, 0xFF}, vers[] = {0, 1, 0xFF};
    int n = 0, bad = 0;
    DVDDiskID ids1[400], ids2[400]; (void) ids2;
    int cnt = 0;
    for (auto g : games) for (auto c : comps) for (auto d : nums) for (auto v : vers) {
        DVDDiskID x, y;
        const char* gg = (g && g[0] == 0) ? NULL : g;
        DVDGenerateDiskID(&x, gg, c, d, v); orig_DVDGenerateDiskID(&y, gg, c, d, v);
        if (memcmp(&x, &y, sizeof x) != 0) bad++;
        if (cnt < 400) ids1[cnt++] = x;
    }
    for (int i = 0; i < cnt; i += 3) for (int j = 0; j < cnt; j++) {
        n++;
        if (DVDCompareDiskID(&ids1[i], &ids1[j]) != orig_DVDCompareDiskID(&ids1[i], &ids1[j])) bad++;
    }
    CHECK(bad == 0 && n > 1000);
    // the game's flow: ask for disc 2 and the id the game sees changes, only after the pump
    DVDDiskID want; DVDGenerateDiskID(&want, "G4BE", "08", 1, 0xFF);
    CHECK(DVDCompareDiskID(DVDGetCurrentDiskID(), &want) == 0);          // disc 1 is in, disc 2 wanted
    DVDCommandBlock cb; memset(&cb, 0, sizeof cb); g_changeDone = 0;
    CHECK(DVDChangeDiskAsync(&cb, &want, ChangeCb) == TRUE);
    CHECK(g_changeDone == 0 && DVDGetCurrentDiskID()->diskNumber == 0);
    CHECK(DVDGetDriveStatus() == DVD_STATE_END);
    CHECK(g_changeDone == 1 && DVDGetCurrentDiskID()->diskNumber == 1 && DVDGetCommandBlockStatus(&cb) == DVD_STATE_END);
    CHECK(DVDCompareDiskID(DVDGetCurrentDiskID(), &want) == 1);
    // NULL callback (the game passes 0)
    DVDCommandBlock cb2; memset(&cb2, 0, sizeof cb2); DVDGenerateDiskID(&want, "G4BE", "08", 0, 0xFF);
    DVDChangeDiskAsync(&cb2, &want, NULL); XDvd_Pump(); CHECK(DVDGetCurrentDiskID()->diskNumber == 0);
    CHECK((uintptr_t) DVDGetFSTLocation() > 0x80000000u && (uintptr_t) DVDGetFSTLocation() < 0x81800000u);
    CHECK(DVDOpenDir("x", NULL) == 0 && DVDReadDir(NULL, NULL) == 0 && DVDCloseDir(NULL) == 0);
}

// ---- ARAM / ARQ -----------------------------------------------------------------------------------------
static int g_arqLog[16], g_arqN; static ARQRequest* g_arqReq[16];
static void ArqCb(u32 req) { if (g_arqN < 16) { g_arqReq[g_arqN] = (ARQRequest*) (uintptr_t) req; g_arqLog[g_arqN] = (int) ((ARQRequest*) (uintptr_t) req)->owner; } g_arqN++; }
static int g_flag; static void FlagCb(u32) { g_flag = 0; }
static ARQRequest g_repost; static int g_repostN;
static void RepostCb(u32) { if (++g_repostN < 3) ARQPostRequest(&g_repost, 7, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, RepostCb); }
static void PostMoreCb(u32) { static ARQRequest extra; ARQPostRequest(&extra, 99, 0, 1, (u32) (uintptr_t) g_buf[0], 0x8000, 32, ArqCb); }

static void TestAram()
{
    Reset();
    u32 stack[3] = {0, 0, 0};
    CHECK(!ARCheckInit() && !ARQCheckInit());
    CHECK(ARInit(stack, 3) == 0x4000 && ARCheckInit() && ARGetBaseAddress() == 0x4000);
    CHECK(ARInit(NULL, 0) == 0x4000);                                  // second call changes nothing
    CHECK(ARGetSize() == 0x1000000 && ARGetInternalSize() == 0x1000000);
    CHECK(ARAlloc(0x1000) == 0x4000 && ARAlloc(0x2000) == 0x5000 && stack[0] == 0x1000 && stack[1] == 0x2000);
    unsigned e = XDvd_ErrorCount();
    CHECK(ARAlloc(33) == 0 && ARAlloc(0x2000000) == 0 && XDvd_ErrorCount() == e + 2);
    CHECK(ARAlloc(0x20) == 0x7000 && ARAlloc(0x20) == 0);              // 3 blocks only
    u32 len = 0; CHECK(ARFree(&len) == 0x7000 && len == 0x20);
    CHECK(ARFree(NULL) == 0x5000 && ARFree(&len) == 0x4000 && len == 0x1000);
    CHECK(XAram_Memory() != NULL);

    ARQInit(); CHECK(ARQCheckInit());
    CHECK(ARQGetChunkSize() == 4096); ARQSetChunkSize(100); CHECK(ARQGetChunkSize() == 128);
    ARQRequest r1, r2, r3, r4;
    for (int i = 0; i < 64; i++) g_buf[0][i] = (unsigned char) (i + 1);
    // deferred, high priority first, FIFO inside a list, callback argument is the request
    g_arqN = 0;
    ARQPostRequest(&r1, 1, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_LOW, (u32) (uintptr_t) g_buf[0], 0x4000, 64, ArqCb);
    ARQPostRequest(&r2, 2, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_HIGH, (u32) (uintptr_t) g_buf[0] + 32, 0x4040, 32, ArqCb);
    ARQPostRequest(&r3, 3, ARQ_TYPE_MRAM_TO_ARAM, ARQ_PRIORITY_HIGH, (u32) (uintptr_t) g_buf[0], 0x4080, 32, ArqCb);
    CHECK(g_arqN == 0 && XDvd_Pending() && XAram_Memory()[0x4000] == 0);
    XDvd_Pump();
    CHECK(g_arqN == 3 && g_arqLog[0] == 2 && g_arqLog[1] == 3 && g_arqLog[2] == 1);
    CHECK(g_arqReq[0] == &r2 && g_arqReq[2] == &r1 && !XDvd_Pending());
    CHECK(memcmp(XAram_Memory() + 0x4000, g_buf[0], 64) == 0);
    CHECK(memcmp(XAram_Memory() + 0x4040, g_buf[0] + 32, 32) == 0);   // r2 ran after r3? no: r2 first, r3 overwrote 0x4080 only
    // read back
    memset(g_buf[1], 0, 64); g_arqN = 0;
    ARQPostRequest(&r4, 4, ARQ_TYPE_ARAM_TO_MRAM, ARQ_PRIORITY_LOW, 0x4000, (u32) (uintptr_t) g_buf[1], 32, ArqCb); XDvd_Pump();
    CHECK(g_arqN == 1 && memcmp(g_buf[1], XAram_Memory() + 0x4000, 32) == 0 && g_buf[1][32] == 0);
    // the "busy flag" pattern
    g_flag = 0; ARQPostRequest(&r1, 0, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, FlagCb); g_flag = 1; XDvd_Pump(); CHECK(g_flag == 0);
    // NULL callback is fine; flush drops without calling; remove by request / owner
    g_arqN = 0;
    ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, NULL);
    ARQPostRequest(&r2, 2, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQPostRequest(&r3, 3, 0, 0, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQFlushQueue(); XDvd_Pump(); CHECK(g_arqN == 0 && !XDvd_Pending());
    ARQPostRequest(&r1, 5, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQPostRequest(&r2, 6, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQPostRequest(&r3, 5, 0, 0, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQPostRequest(&r4, 8, 0, 0, (u32) (uintptr_t) g_buf[0], 0x4000, 32, ArqCb);
    ARQRemoveRequest(&r2); ARQRemoveOwnerRequest(5);
    XDvd_Pump(); CHECK(g_arqN == 1 && g_arqLog[0] == 8);
    // a callback may post its own request again, and posts made by callbacks run in the same pump
    g_repostN = 0; ARQPostRequest(&g_repost, 7, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, RepostCb); XDvd_Pump(); CHECK(g_repostN == 3);
    g_arqN = 0; ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, PostMoreCb); XDvd_Pump();
    CHECK(g_arqN == 1 && g_arqLog[0] == 99);
    // out of range: refused and counted, the callback still comes so nobody waits forever
    e = XDvd_ErrorCount(); g_arqN = 0; memset(XAram_Memory() + 0xFFFFE0, 0x5A, 32);
    ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0xFFFFE0 + 0x20, 32, ArqCb); XDvd_Pump();
    CHECK(g_arqN == 1 && XDvd_ErrorCount() == e + 1);
    ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0xFFFFE0, 32, ArqCb); XDvd_Pump();   // last block: fine
    CHECK(XDvd_ErrorCount() == e + 1 && memcmp(XAram_Memory() + 0xFFFFE0, g_buf[0], 32) == 0);
    ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0, 0xFFFFFFE0u, ArqCb); XDvd_Pump(); CHECK(XDvd_ErrorCount() == e + 2);   // length overflow
    // bad arguments to the post itself are dropped
    g_arqN = 0; ARQPostRequest(&r1, 1, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 33, ArqCb); ARQPostRequest(&r1, 1, 5, 1, 0, 0, 32, ArqCb);
    ARQPostRequest(&r1, 1, 0, 7, 0, 0, 32, ArqCb); ARQPostRequest(NULL, 1, 0, 1, 0, 0, 32, ArqCb); XDvd_Pump();
    CHECK(g_arqN == 0 && XDvd_ErrorCount() == e + 6);
    ARQReset(); CHECK(!ARQCheckInit());
}

// the real flow of dvd.cpp: a read finishes and its callback starts the ARAM copy; both ride one pump
static ARQRequest g_flowReq; static int g_flowDone;
static void FlowArq(u32) { g_flowDone = 1; }
static void FlowRead(s32, DVDFileInfo*) { ARQPostRequest(&g_flowReq, 0, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, FlowArq); }
static int g_spinPumped;
static void SpinCb(u32) { g_flowDone = 2; }
static void TestFlowAndSpin()
{
    Reset(); AddFile("f.bin", 4096); u32 st[2]; ARInit(st, 2); ARQInit();
    DVDFileInfo a; memset(&a, 0, sizeof a); DVDOpen("f.bin", &a); g_flowDone = 0;
    DVDReadAsyncPrio(&a, g_buf[0], 32, 0, FlowRead, 2);
    XDvd_Pump();
    CHECK(g_flowDone == 1 && memcmp(XAram_Memory() + 0x4000, g_files[0].data, 32) == 0);
    // the spin loop of cAram::DmaTrans: post, then pump until the flag moves; also from inside a callback
    g_flowDone = 0; ARQRequest r;
    ARQPostRequest(&r, 0, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, SpinCb);
    int guard = 0; while (g_flowDone == 0 && guard++ < 10) XDvd_Pump();
    CHECK(g_flowDone == 2 && guard == 1);
}
static int g_spinResult;
static void SpinInsideCb(s32, DVDFileInfo*)
{
    static ARQRequest rr; g_flowDone = 0;
    ARQPostRequest(&rr, 0, 0, 1, (u32) (uintptr_t) g_buf[0], 0x4000, 32, SpinCb);
    int guard = 0; while (g_flowDone == 0 && guard++ < 10) XDvd_Pump();
    g_spinResult = g_flowDone;
}
static void TestNestedSpin()
{
    Reset(); AddFile("f.bin", 4096); DVDFileInfo a; memset(&a, 0, sizeof a); DVDOpen("f.bin", &a);
    g_flowDone = 0; DVDReadAsyncPrio(&a, g_buf[0], 32, 0, SpinInsideCb, 2); XDvd_Pump();
    CHECK(g_spinResult == 2);     // a nested pump inside a callback made progress, no hang
}

int main()
{
    TestPaths(); TestTableFull(); TestRead(); TestCallbacksOrdering(); TestCancel(); TestFatal(); TestDiskId(); TestAram(); TestFlowAndSpin(); TestNestedSpin();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
