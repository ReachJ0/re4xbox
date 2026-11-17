// xbox/dvd_xbox: Xbox replacement for the Dolphin SDK DVD library (src/lib/dvd*.c, dvdfs.c, dvdidutils.c).
//
// Provides the DVD calls the game makes and nothing else:
//   DVDInit DVDConvertPathToEntrynum DVDFastOpen DVDOpen DVDClose DVDReadAsyncPrio DVDCancelAsync DVDCancel
//   DVDCancelAll DVDGetCommandBlockStatus DVDGetDriveStatus DVDGetTransferredSize DVDGetFSTLocation
//   DVDGetCurrentDiskID DVDChangeDiskAsync DVDCompareDiskID DVDGenerateDiskID
//   DVDOpenDir DVDReadDir DVDCloseDir (always fail: only the debug screens use them)
// Link this file INSTEAD of src/lib/dvd.c, dvdfs.c, dvdidutils.c, dvdlow.c, dvdqueue.c, dvderror.c,
// dvdFatal.c, fstload.c (and ar_xbox.cpp instead of ar.c, arq.c).
//
// How it works: the disc is a folder of plain files reached through XDvdBackend_* (xbox/xdvd_backend.h).
// An "entry number" is an index into a table filled the first time a path is looked up (entry 0 is the
// root); DVDFileInfo.startAddr holds it, since there are no disc addresses. A read is queued in one of
// four priority lists (0 first, first come first served inside a list), the way the SDK queues commands;
// the head of the queue is the "executing" command (state BUSY), the rest are WAITING.
//
// Nothing finishes inside the call that starts it. XDvd_Pump() (xbox/xdvd.h) performs the queued commands
// and then calls the game back, because the game relies on callbacks arriving after the call returns.
// One consequence: DVDGetCommandBlockStatus does not pump, so a stream polling its own block sees BUSY or
// WAITING until the pump has run; DVDGetDriveStatus does pump, because the game loops on it.
//
// Differences from the real library, all deliberate: calls the SDK would stop on in OSPanic (a read that
// is misaligned or outside the file, a full entry table) are refused and counted (XDvd_ErrorCount); a
// read that fails in the backend is a fatal drive error, sticky like the SDK's; there is one disc, so
// DVDChangeDiskAsync only switches the disc id the game sees; no cover or retry states exist.
//
// Only compiled for the Xbox build (RE4_XBOX); it has no meaning on the GameCube target.
#ifdef RE4_XBOX

#include <dolphin/dvd.h>

#include <stdint.h>
#include <string.h>

#include "xbox/xdvd.h"
#include "xbox/xdvd_backend.h"

#define MAX_ENTRIES 4096
#define NPRIO 4
#define PUMP_LIMIT 256
#define PUMP_DEPTH 8

// DVDGetFSTLocation: where the SDK leaves the file system table, near the top of the 24 MB of RAM. The
// game only subtracts 0x80000000 from it to size its heap (cDvd::FstSize), so any plausible address will
// do until the OS layer decides the memory map.
#define FST_ADDRESS XMEM_ADDR(0x81700000u)

#define CB_CALLBACK_CANCELED_WAITING (-3)

namespace {

struct Entry {
    unsigned int size;
    int isDir;
    char path[XDVD_PATH_MAX];
};

Entry s_ent[MAX_ENTRIES];
int s_nent = 0;
int s_inited = 0;
int s_backendOk = 0;
unsigned int s_errors = 0;

DVDDiskID s_diskId;

// queues of waiting commands (linked through DVDCommandBlock::next) and the executing one
DVDCommandBlock* s_head[NPRIO];
DVDCommandBlock* s_tail[NPRIO];
DVDCommandBlock* s_exec = 0;
int s_holding = 0;  // >0 while a callback runs: new commands wait instead of becoming the executing one
int s_fatal = 0;
int s_pumpDepth = 0;

// a cancel of the executing command, completed by the next pump (the SDK breaks the transfer and the
// callbacks arrive later)
DVDCommandBlock* s_cancelBlock = 0;
DVDCBCallback s_cancelCb = 0;

void EnsureInit()
{
    if (s_inited) {
        return;
    }
    s_inited = 1;
    s_backendOk = XDvdBackend_Init();
    memset(&s_diskId, 0, sizeof(s_diskId));
    memcpy(s_diskId.gameName, "G4BE", 4);
    memcpy(s_diskId.company, "08", 2);
    s_nent = 1;  // entry 0: the root directory
    s_ent[0].size = 0;
    s_ent[0].isDir = 1;
    s_ent[0].path[0] = 0;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c; }

int SameName(const char* a, const char* b)
{
    while (*a && *b) {
        if (Lower(*a++) != Lower(*b++)) {
            return 0;
        }
    }
    return *a == *b;
}

// Turns a game path into the backend form. 0 if it can not be (too long, or ".." in it).
int Normalize(const char* in, char* out)
{
    unsigned int n = 0;
    int partStart = 1;

    while (*in == '/' || *in == '\\') {
        in++;
    }
    while (*in) {
        char c = *in++;
        if (c == '\\') {
            c = '/';
        }
        if (c == '/') {
            if (partStart) {  // "//" or a leading "./"
                continue;
            }
            partStart = 1;
            if (n + 1 >= XDVD_PATH_MAX) {
                return 0;
            }
            out[n++] = '/';
            continue;
        }
        if (partStart && c == '.') {
            char d = in[0];
            if (d == '/' || d == '\\' || d == 0) {  // "."
                continue;
            }
            if (d == '.' && (in[1] == '/' || in[1] == '\\' || in[1] == 0)) {  // ".."
                return 0;
            }
        }
        partStart = 0;
        if (n + 1 >= XDVD_PATH_MAX) {
            return 0;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '/') {  // trailing slash
        n--;
    }
    out[n] = 0;
    return 1;
}

// ---- command queue ---------------------------------------------------------------------------------

void Promote()
{
    for (int p = 0; p < NPRIO; p++) {
        if (s_head[p]) {
            DVDCommandBlock* b = s_head[p];
            s_head[p] = b->next;
            if (!s_head[p]) {
                s_tail[p] = 0;
            }
            b->next = 0;
            b->state = DVD_STATE_BUSY;
            s_exec = b;
            return;
        }
    }
}

void Post(DVDCommandBlock* b, int prio)
{
    if (prio < 0) {
        prio = 0;
    }
    if (prio >= NPRIO) {
        prio = NPRIO - 1;
    }
    b->next = 0;
    b->prev = 0;
    b->state = DVD_STATE_WAITING;
    b->transferredSize = 0;
    b->currTransferSize = 0;
    if (s_tail[prio]) {
        s_tail[prio]->next = b;
    } else {
        s_head[prio] = b;
    }
    s_tail[prio] = b;
    if (!s_exec && !s_holding) {
        Promote();
    }
}

// Takes `b` out of the waiting lists. 1 if it was there.
int Unlink(DVDCommandBlock* b)
{
    for (int p = 0; p < NPRIO; p++) {
        DVDCommandBlock* prev = 0;
        for (DVDCommandBlock* c = s_head[p]; c; prev = c, c = c->next) {
            if (c == b) {
                if (prev) {
                    prev->next = c->next;
                } else {
                    s_head[p] = c->next;
                }
                if (s_tail[p] == c) {
                    s_tail[p] = prev;
                }
                c->next = 0;
                return 1;
            }
        }
    }
    return 0;
}

int Queued()
{
    for (int p = 0; p < NPRIO; p++) {
        if (s_head[p]) {
            return 1;
        }
    }
    return 0;
}

void Callback(DVDCommandBlock* b, s32 result)
{
    if (b->callback) {
        s_holding++;
        b->callback(result, b);
        s_holding--;
    }
}

void CallbackFn(DVDCBCallback cb, s32 result, DVDCommandBlock* b)
{
    if (cb) {
        s_holding++;
        cb(result, b);
        s_holding--;
    }
}

void ReadDone(s32 result, DVDCommandBlock* b)
{
    DVDFileInfo* fi = (DVDFileInfo*) b;  // the command block is the first member
    if (fi->callback) {
        fi->callback(result, fi);
    }
}

// Performs the executing command, then calls back. Returns 0 if there was nothing to do.
int StepDvd()
{
    DVDCommandBlock* b;
    s32 result = 0;
    int state = DVD_STATE_END;

    if (!s_exec) {
        Promote();
    }
    if (!s_exec) {
        return 0;
    }
    b = s_exec;
    s_exec = 0;

    if (b == s_cancelBlock) {
        DVDCBCallback cc = s_cancelCb;
        s_cancelBlock = 0;
        s_cancelCb = 0;
        b->state = DVD_STATE_CANCELED;
        Callback(b, DVD_RESULT_CANCELED);
        CallbackFn(cc, 0, b);
    } else {
        if (s_fatal) {
            state = DVD_STATE_FATAL_ERROR;
            result = DVD_RESULT_FATAL_ERROR;
        } else if (b->command == DVD_COMMAND_READ) {
            DVDFileInfo* fi = (DVDFileInfo*) b;
            const Entry& e = s_ent[fi->startAddr];
            unsigned int off = b->offset;
            unsigned int len = b->length;
            unsigned int avail = off < e.size ? (e.size - off < len ? e.size - off : len) : 0;
            unsigned int got = 0;

            if (avail > 0 && (!XDvdBackend_Read(e.path, off, b->addr, avail, &got) || got != avail)) {
                s_fatal = 1;
                state = DVD_STATE_FATAL_ERROR;
                result = DVD_RESULT_FATAL_ERROR;
            } else {
                // the real drive delivers the following file's bytes up to the 32 byte boundary; zeros here
                memset((unsigned char*) b->addr + avail, 0, len - avail);
                b->transferredSize = len;
                b->currTransferSize = len;
                result = (s32) len;
            }
        } else if (b->command == DVD_COMMAND_CHANGE_DISK) {
            if (b->id) {
                s_diskId = *b->id;
            }
        }
        b->state = state;
        Callback(b, result);  // holds back promotion: the next command is chosen after it, by priority
    }
    if (!s_exec) {
        Promote();
    }
    return 1;
}

// Finishes `b` as cancelled right now. `waiting` picks the result the SDK reports (-3 / -6).
void CancelNow(DVDCommandBlock* b, int waiting)
{
    b->state = DVD_STATE_CANCELED;
    Callback(b, waiting ? CB_CALLBACK_CANCELED_WAITING : DVD_RESULT_CANCELED);
}

}  // namespace

extern "C" {

// ---- the layer's own entry points (xbox/xdvd.h) --------------------------------------------------------

void XDvd_CountError(void) { s_errors++; }

unsigned int XDvd_ErrorCount(void) { return s_errors; }

void XDvd_Pump(void)
{
    if (s_pumpDepth >= PUMP_DEPTH) {
        return;
    }
    s_pumpDepth++;
    for (int n = 0; n < PUMP_LIMIT; n++) {
        int did = StepDvd();
        did |= XAram_Service();
        if (!did) {
            break;
        }
    }
    s_pumpDepth--;
}

int XDvd_Pending(void) { return s_exec != 0 || Queued() || XAram_Pending(); }

void XDvd_Reset(void)
{
    memset(s_ent, 0, sizeof(s_ent));
    memset(s_head, 0, sizeof(s_head));
    memset(s_tail, 0, sizeof(s_tail));
    memset(&s_diskId, 0, sizeof(s_diskId));
    s_nent = 0;
    s_inited = 0;
    s_backendOk = 0;
    s_errors = 0;
    s_exec = 0;
    s_holding = 0;
    s_fatal = 0;
    s_pumpDepth = 0;
    s_cancelBlock = 0;
    s_cancelCb = 0;
    XAram_Reset();
}

// ---- DVD: files ------------------------------------------------------------------------------------------

void DVDInit(void) { EnsureInit(); }

s32 DVDConvertPathToEntrynum(const char* pathPtr)
{
    char path[XDVD_PATH_MAX];
    unsigned int size = 0;
    int isDir = 0;

    EnsureInit();
    if (!pathPtr || !Normalize(pathPtr, path)) {
        return -1;
    }
    if (path[0] == 0) {
        return 0;
    }
    for (int i = 1; i < s_nent; i++) {
        if (SameName(s_ent[i].path, path)) {
            return i;
        }
    }
    if (!s_backendOk || !XDvdBackend_Stat(path, &size, &isDir)) {
        return -1;
    }
    if (s_nent >= MAX_ENTRIES) {
        XDvd_CountError();
        return -1;
    }
    s_ent[s_nent].size = size;
    s_ent[s_nent].isDir = isDir;
    strcpy(s_ent[s_nent].path, path);
    return s_nent++;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    EnsureInit();
    if (!fileInfo || entrynum < 0 || entrynum >= s_nent || s_ent[entrynum].isDir) {
        return FALSE;
    }
    fileInfo->startAddr = (u32) entrynum;
    fileInfo->length = s_ent[entrynum].size;
    fileInfo->callback = 0;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo)
{
    s32 e = DVDConvertPathToEntrynum(fileName);

    if (e < 0) {
        return FALSE;
    }
    return DVDFastOpen(e, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    if (fileInfo) {
        DVDCancel(&fileInfo->cb);
    }
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    DVDCommandBlock* b;

    EnsureInit();
    // each of these is an OSPanic in the SDK
    if (!fileInfo || !addr || ((uintptr_t) addr & 31) || length < 0 || (length & 31) || (offset & 3) || offset < 0 ||
        (u32) offset > fileInfo->length || (u32) offset + (u32) length >= fileInfo->length + DVD_MIN_TRANSFER_SIZE ||
        fileInfo->startAddr == 0 || fileInfo->startAddr >= (u32) s_nent || fileInfo->cb.state == DVD_STATE_WAITING ||
        fileInfo->cb.state == DVD_STATE_BUSY) {
        XDvd_CountError();
        return FALSE;
    }
    fileInfo->callback = callback;
    b = &fileInfo->cb;
    b->command = DVD_COMMAND_READ;
    b->addr = addr;
    b->length = (u32) length;
    b->offset = (u32) offset;
    b->callback = ReadDone;
    b->id = 0;
    Post(b, (int) prio);
    return TRUE;
}

// Blocking read (SDK dvdfs.c): the number of bytes read, -1 on a drive error or a refused request, -3 when the
// command was cancelled. The SDK sleeps the thread until the interrupt handler finishes the command; here the
// pump runs it, which completes the queue head per call, so the loop ends after as many pumps as there are
// queued commands.
s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio)
{
    s32 state;
    int guard;

    if (!DVDReadAsyncPrio(fileInfo, addr, length, offset, 0, prio)) {
        return -1;
    }
    for (guard = 0; guard < 100000; guard++) {
        state = ((volatile DVDCommandBlock*) &fileInfo->cb)->state;
        if (state == DVD_STATE_END) {
            return (s32) fileInfo->cb.transferredSize;
        }
        if (state == DVD_STATE_FATAL_ERROR) {
            return -1;
        }
        if (state == DVD_STATE_CANCELED) {
            return -3;
        }
        XDvd_Pump();
    }
    return -1;
}

s32 DVDGetTransferredSize(DVDFileInfo* fileinfo)
{
    return fileinfo->cb.state == DVD_STATE_WAITING ? 0 : (s32) fileinfo->cb.transferredSize;
}

// ---- DVD: drive and commands -------------------------------------------------------------------------

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block)
{
    return block->state == DVD_STATE_COVER_CLOSED ? 1 : block->state;
}

s32 DVDGetDriveStatus(void)
{
    XDvd_Pump();
    if (s_fatal) {
        return DVD_STATE_FATAL_ERROR;
    }
    return s_exec ? DVDGetCommandBlockStatus(s_exec) : DVD_STATE_END;
}

int DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
    switch (block->state) {
    case DVD_STATE_FATAL_ERROR:
    case DVD_STATE_END:
    case DVD_STATE_CANCELED:
        CallbackFn(callback, 0, block);
        return TRUE;
    case DVD_STATE_BUSY:
        if (block != s_exec) {  // not ours (a block from before a reset): nothing to cancel
            CallbackFn(callback, 0, block);
            return TRUE;
        }
        if (s_cancelBlock) {  // one cancel at a time, as in the SDK
            return FALSE;
        }
        s_cancelBlock = block;
        s_cancelCb = callback;
        return TRUE;
    case DVD_STATE_WAITING:
        if (Unlink(block)) {
            CancelNow(block, 1);
        }
        CallbackFn(callback, 0, block);
        return TRUE;
    default:
        CallbackFn(callback, 0, block);
        return TRUE;
    }
}

// Cancels and waits. Nothing runs in the background here, so "waiting" is finishing the cancel now.
s32 DVDCancel(volatile DVDCommandBlock* block)
{
    DVDCommandBlock* b = (DVDCommandBlock*) block;

    if (b->state == DVD_STATE_BUSY && b == s_exec) {
        s_exec = 0;
        if (b == s_cancelBlock) {
            DVDCBCallback cc = s_cancelCb;
            s_cancelBlock = 0;
            s_cancelCb = 0;
            CancelNow(b, 0);
            CallbackFn(cc, 0, b);
        } else {
            CancelNow(b, 0);
        }
        if (!s_exec && !s_holding) {
            Promote();
        }
        return 0;
    }
    if (b->state == DVD_STATE_WAITING && Unlink(b)) {
        CancelNow(b, 1);
    }
    return 0;
}

s32 DVDCancelAll(void)
{
    DVDCommandBlock* batch[256];
    int n = 0;
    int nExec = 0;

    // The commands present now. Callbacks may queue more; those are left for the next pump.
    if (s_exec) {
        batch[n++] = s_exec;
        nExec = 1;
        s_exec = 0;
    }
    for (int p = 0; p < NPRIO; p++) {
        for (DVDCommandBlock* c = s_head[p]; c && n < 256; c = c->next) {
            batch[n++] = c;
        }
    }
    for (int i = nExec; i < n; i++) {
        Unlink(batch[i]);
    }
    s_cancelBlock = 0;
    s_cancelCb = 0;
    for (int i = 0; i < n; i++) {
        CancelNow(batch[i], i >= nExec);  // the running one reports -6, the waiting ones -3
    }
    if (!s_exec && !s_holding) {
        Promote();
    }
    return 0;
}

int DVDChangeDiskAsync(DVDCommandBlock* block, DVDDiskID* id, DVDCBCallback callback)
{
    EnsureInit();
    block->command = DVD_COMMAND_CHANGE_DISK;
    block->id = id;
    block->callback = callback;
    Post(block, 2);
    return TRUE;
}

// ---- DVD: disc id ------------------------------------------------------------------------------------

DVDDiskID* DVDGetCurrentDiskID(void)
{
    EnsureInit();
    return &s_diskId;
}

void* DVDGetFSTLocation(void) { return (void*) (uintptr_t) FST_ADDRESS; }

// same rules as src/lib/dvdidutils.c (without its debug asserts)
int DVDCompareDiskID(const DVDDiskID* id1, const DVDDiskID* id2)
{
    if (id1->gameName[0] != 0 && id2->gameName[0] != 0 && strncmp(id1->gameName, id2->gameName, 4) != 0) {
        return 0;
    }
    if (id1->company[0] == 0 || id2->company[0] == 0 || strncmp(id1->company, id2->company, 2) != 0) {
        return 0;
    }
    if (id1->diskNumber != 0xFF && id2->diskNumber != 0xFF && id1->diskNumber != id2->diskNumber) {
        return 0;
    }
    if (id1->gameVersion != 0xFF && id2->gameVersion != 0xFF && id1->gameVersion != id2->gameVersion) {
        return 0;
    }
    return 1;
}

DVDDiskID* DVDGenerateDiskID(DVDDiskID* id, const char* game, const char* company, u8 diskNum, u8 version)
{
    memset(id, 0, sizeof(DVDDiskID));
    if (game != NULL) {
        strncpy(id->gameName, game, 4);
    }
    if (company != NULL) {
        strncpy(id->company, company, 2);
    }
    id->diskNumber = diskNum;
    id->gameVersion = version;
    return id;
}

// ---- DVD: directories (only the debug screens use them) -----------------------------------------------

int DVDOpenDir(const char*, DVDDir*) { return 0; }
int DVDReadDir(DVDDir*, DVDDirEntry*) { return 0; }
int DVDCloseDir(DVDDir*) { return 0; }

}  // extern "C"

#endif  // RE4_XBOX
