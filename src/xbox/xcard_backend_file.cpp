// xbox/xcard_backend_file: XCardBackend_* on top of stdio, one image file per slot on the console's disk.
//
// Slot A is "<save dir>/RE4_A.CARD" on the hard disk. Slot B has no storage, so the game sees an empty
// second slot (a memory unit in a controller port could back it later, by returning 1 from Present for
// it and pointing SlotPath at the unit).
//
// Assumptions about nxdk, none of them tried on hardware: nxMountDrive (nxdk/mount.h) mounts the E:
// partition, CreateDirectoryA (windows.h) creates the folder, and fopen / fseek / fread / fwrite / fflush /
// rename / remove work on it. Flush is fflush only: it hands the data to the OS, and this layer does not
// call anything stronger, so a power cut right after a save can still lose it.
#ifdef RE4_XBOX

#include "xbox/xcard_backend.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef NXDK
#include <windows.h>
#include <nxdk/mount.h>
#endif

// Where the images live. E: is the console's data partition; the folder is created on first use.
#define SAVE_DIR "E:\\UDATA\\RE4"

static FILE* s_file[XCARD_SLOTS];
static int s_ready = 0;

static const char* SlotPath(int slot)
{
    switch (slot) {
    case 0:
        return SAVE_DIR "\\RE4_A.CARD";
    default:
        return NULL;  // slot B: no storage
    }
}

static int HasStorage(int slot) { return s_ready && slot >= 0 && slot < XCARD_SLOTS && SlotPath(slot) != NULL; }

// The open image for `slot`, opened on first use (created if missing). NULL on failure.
static FILE* Image(int slot)
{
    if (!HasStorage(slot)) {
        return NULL;
    }
    if (!s_file[slot]) {
        s_file[slot] = fopen(SlotPath(slot), "r+b");
        if (!s_file[slot]) {
            s_file[slot] = fopen(SlotPath(slot), "w+b");
        }
    }
    return s_file[slot];
}

extern "C" {

int XCardBackend_Init(void)
{
#ifdef NXDK
    // The data partition is not mounted by default
    if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
        return 0;
    }
    CreateDirectoryA("E:\\UDATA", NULL);
    CreateDirectoryA(SAVE_DIR, NULL);
#endif
    s_ready = 1;
    return 1;
}

int XCardBackend_Present(int slot) { return Image(slot) != NULL; }

int XCardBackend_Size(int slot, unsigned int* size)
{
    FILE* f = Image(slot);
    long end;

    if (!f || !size || fseek(f, 0, SEEK_END) != 0) {
        return 0;
    }
    end = ftell(f);
    if (end < 0) {
        return 0;
    }
    *size = (unsigned int) end;
    return 1;
}

int XCardBackend_Read(int slot, unsigned int off, void* buf, unsigned int len)
{
    FILE* f = Image(slot);
    unsigned int size;

    if (!f || !XCardBackend_Size(slot, &size) || off > size || len > size - off) {
        return 0;
    }
    return fseek(f, (long) off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

int XCardBackend_Write(int slot, unsigned int off, const void* buf, unsigned int len)
{
    FILE* f = Image(slot);
    unsigned int size;

    if (!f || !XCardBackend_Size(slot, &size)) {
        return 0;
    }
    if (off > size) {  // a gap: zero-fill it, as the interface promises
        static const unsigned char zeros[512] = {0};
        unsigned int gap = off - size;

        if (fseek(f, 0, SEEK_END) != 0) {
            return 0;
        }
        while (gap > 0) {
            unsigned int n = gap < sizeof(zeros) ? gap : (unsigned int) sizeof(zeros);
            if (fwrite(zeros, 1, n, f) != n) {
                return 0;
            }
            gap -= n;
        }
    }
    return fseek(f, (long) off, SEEK_SET) == 0 && fwrite(buf, 1, len, f) == len;
}

int XCardBackend_Truncate(int slot, unsigned int size)
{
    // stdio has no truncate: rewrite the kept part into a fresh file
    FILE* f = Image(slot);
    unsigned int cur;
    unsigned char chunk[512];
    unsigned int pos = 0;
    FILE* in;
    FILE* out;
    char tmp[64];

    if (!f || !XCardBackend_Size(slot, &cur)) {
        return 0;
    }
    if (size >= cur) {
        return 1;
    }
    if (strlen(SlotPath(slot)) + 5 > sizeof(tmp)) {
        return 0;
    }
    strcpy(tmp, SlotPath(slot));
    strcat(tmp, ".tmp");

    in = f;
    out = fopen(tmp, "wb");
    if (!out) {
        return 0;
    }
    while (pos < size) {
        unsigned int n = size - pos < sizeof(chunk) ? size - pos : (unsigned int) sizeof(chunk);
        if (fseek(in, (long) pos, SEEK_SET) != 0 || fread(chunk, 1, n, in) != n || fwrite(chunk, 1, n, out) != n) {
            fclose(out);
            remove(tmp);
            return 0;
        }
        pos += n;
    }
    if (fclose(out) != 0) {
        remove(tmp);
        return 0;
    }
    fclose(in);
    s_file[slot] = NULL;
    remove(SlotPath(slot));
    if (rename(tmp, SlotPath(slot)) != 0) {
        return 0;  // the image is gone; the next mount sees a new, empty card
    }
    return Image(slot) != NULL;
}

int XCardBackend_Flush(int slot)
{
    FILE* f = Image(slot);

    return f != NULL && fflush(f) == 0;
}

unsigned int XCardBackend_Seconds2000(void)
{
    time_t now = time(NULL);
    // 2000-01-01 00:00:00 UTC in Unix seconds
    const time_t epoch2000 = 946684800;
    return now > epoch2000 ? (unsigned int) (now - epoch2000) : 0;
}

}  // extern "C"

#endif  // RE4_XBOX
