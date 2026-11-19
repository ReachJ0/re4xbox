// xbox/xdvd_backend_file: XDvdBackend_* on top of stdio, reading the game's files from one folder.
//
// The folder (XDVD_ROOT) holds the disc contents with the disc's own paths, e.g. "<root>/bio4/xxx.dat".
// On the Xbox it is the folder the .xbe was started from, D:\ in nxdk's naming.
//
// Assumptions about nxdk, none of them tried on hardware: D: is already mounted when the game starts
// (otherwise XDvdBackend_Init must mount it with nxMountDrive, like the save backend does for E:), and
// fopen / fseek / fread work on it. Reads go through a small cache of open files, because the game reads
// a few big files in turns (streams, data) and reopening on every read would be slow on the console.
#ifdef RE4_XBOX

#include "xbox/xdvd_backend.h"

#include <stdio.h>
#include <string.h>

#ifdef NXDK
#define XDVD_ROOT "D:\\"
#define XDVD_SEP '\\'
#else
#define XDVD_ROOT "./disc/"
#define XDVD_SEP '/'
#endif

#ifdef NXDK
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define CACHE_FILES 4
#define FULL_MAX 320

namespace {

char s_root[FULL_MAX] = XDVD_ROOT;
char s_base[FULL_MAX] = XDVD_ROOT;   // the folder given by XDVD_ROOT / SetRoot; disc folders live inside it

struct Open {
    FILE* f;
    char path[XDVD_PATH_MAX];
    unsigned int stamp;
};
Open s_open[CACHE_FILES];
unsigned int s_clock = 0;

void CloseAll()
{
    for (int i = 0; i < CACHE_FILES; i++) {
        if (s_open[i].f) {
            fclose(s_open[i].f);
        }
        s_open[i].f = NULL;
        s_open[i].path[0] = 0;
    }
}

// root + path with the platform's separator. 0 if it does not fit or tries to climb out of the root.
int FullPath(const char* path, char* out)
{
    size_t rl = strlen(s_root);
    size_t pl = strlen(path);

    if (rl + pl + 1 > FULL_MAX || strstr(path, "..") != NULL) {
        return 0;
    }
    memcpy(out, s_root, rl);
    for (size_t i = 0; i <= pl; i++) {
        out[rl + i] = (path[i] == '/') ? XDVD_SEP : path[i];
    }
    return 1;
}

FILE* Acquire(const char* path)
{
    int victim = 0;

    for (int i = 0; i < CACHE_FILES; i++) {
        if (s_open[i].f && strcmp(s_open[i].path, path) == 0) {
            s_open[i].stamp = ++s_clock;
            return s_open[i].f;
        }
    }
    for (int i = 0; i < CACHE_FILES; i++) {
        if (!s_open[i].f) {
            victim = i;
            break;
        }
        if (s_open[i].stamp < s_open[victim].stamp) {
            victim = i;
        }
    }
    if (s_open[victim].f) {
        fclose(s_open[victim].f);
        s_open[victim].f = NULL;
    }
    {
        char full[FULL_MAX];
        FILE* f;

        if (!FullPath(path, full) || !(f = fopen(full, "rb"))) {
            return NULL;
        }
        s_open[victim].f = f;
        strcpy(s_open[victim].path, path);
        s_open[victim].stamp = ++s_clock;
        return f;
    }
}

}  // namespace

extern "C" {

int XDvdBackend_Init(void)
{
    XDvdBackend_SetDisc(0);
    return 1;
}

void XDvdBackend_SetRoot(const char* dir)
{
    size_t n = dir ? strlen(dir) : 0;

    CloseAll();
    if (n == 0 || n + 2 > FULL_MAX) {
        return;
    }
    memcpy(s_root, dir, n);
    if (s_root[n - 1] != '/' && s_root[n - 1] != '\\') {
        s_root[n++] = XDVD_SEP;
    }
    s_root[n] = 0;
    memcpy(s_base, s_root, n + 1);
}

// The GameCube game has two discs (0 / 1 in the disc id) and asks for the swap itself (cDvd::DiscChange).
// Here the swap is a change of folder: "<base>disc1/" and "<base>disc2/" when they exist; otherwise the base
// folder is used for both (a single-disc layout keeps working, files of the other disc are then "not on disc").
void XDvdBackend_SetDisc(int disc)
{
    char cand[FULL_MAX];
    size_t bl = strlen(s_base);
    int isDir = 0;

    CloseAll();
    memcpy(s_root, s_base, bl + 1);
    if (disc < 0 || disc > 1 || bl + 7 > FULL_MAX) {
        return;
    }
    memcpy(cand, s_base, bl);
    memcpy(cand + bl, "disc", 4);
    cand[bl + 4] = (char) ('1' + disc);
    cand[bl + 5] = XDVD_SEP;
    cand[bl + 6] = 0;
#ifdef NXDK
    {
        DWORD attr = GetFileAttributesA(cand);
        isDir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
#else
    {
        struct stat st;
        isDir = stat(cand, &st) == 0 && S_ISDIR(st.st_mode);
    }
#endif
    if (isDir) {
        memcpy(s_root, cand, bl + 7);
    }
}

int XDvdBackend_Stat(const char* path, unsigned int* size, int* is_dir)
{
    char full[FULL_MAX];

    if (!FullPath(path, full)) {
        return 0;
    }
    if (path[0] == 0) {
        *size = 0;
        *is_dir = 1;
        return 1;
    }
#ifdef NXDK
    {
        DWORD attr = GetFileAttributesA(full);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            return 0;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            *size = 0;
            *is_dir = 1;
            return 1;
        }
    }
#else
    {
        struct stat st;
        if (stat(full, &st) != 0) {
            return 0;
        }
        if (S_ISDIR(st.st_mode)) {
            *size = 0;
            *is_dir = 1;
            return 1;
        }
    }
#endif
    {
        FILE* f = fopen(full, "rb");
        long end;

        if (!f) {
            return 0;
        }
        if (fseek(f, 0, SEEK_END) != 0 || (end = ftell(f)) < 0) {
            fclose(f);
            return 0;
        }
        fclose(f);
        *size = (unsigned int) end;
        *is_dir = 0;
        return 1;
    }
}

int XDvdBackend_Read(const char* path, unsigned int off, void* buf, unsigned int len, unsigned int* got)
{
    FILE* f = Acquire(path);
    size_t n;

    *got = 0;
    if (!f || fseek(f, (long) off, SEEK_SET) != 0) {
        return 0;
    }
    n = fread(buf, 1, len, f);
    if (n < len && ferror(f)) {
        return 0;
    }
    *got = (unsigned int) n;
    return 1;
}

}  // extern "C"

#endif  // RE4_XBOX
