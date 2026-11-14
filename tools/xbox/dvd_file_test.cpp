// Host test for the stdio disc backend (src/xbox/xdvd_backend_file.cpp) on a real temporary folder, then
// the whole layer on top of it. Build and run with tools/xbox/run_dvd_test.sh (it runs both tests).
#include <dolphin/dvd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xbox/xdvd.h"
#include "xbox/xdvd_backend.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void Make(const char* root, const char* rel, unsigned size, unsigned char seed)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE* f = fopen(p, "wb"); for (unsigned i = 0; i < size; i++) fputc((unsigned char) (i * 3 + seed), f); fclose(f);
}

int main(int argc, char** argv)
{
    char root[] = "/tmp/re4_dvdfile_XXXXXX";
    if (!mkdtemp(root)) return 2;
    char p[512]; snprintf(p, sizeof p, "%s/bio4", root); mkdir(p, 0755);
    Make(root, "bio4/a.bin", 5000, 1);
    Make(root, "b.bin", 64, 2);
    Make(root, "empty.bin", 0, 0);
    for (int i = 0; i < 6; i++) { char n[32]; snprintf(n, sizeof n, "m%d.bin", i); Make(root, n, 100 + i, (unsigned char) i); }

    CHECK(XDvdBackend_Init() == 1);
    XDvdBackend_SetRoot(root);
    unsigned sz = 7; int dir = 7;
    CHECK(XDvdBackend_Stat("", &sz, &dir) == 1 && dir == 1);
    CHECK(XDvdBackend_Stat("bio4", &sz, &dir) == 1 && dir == 1 && sz == 0);
    CHECK(XDvdBackend_Stat("bio4/a.bin", &sz, &dir) == 1 && dir == 0 && sz == 5000);
    CHECK(XDvdBackend_Stat("empty.bin", &sz, &dir) == 1 && sz == 0 && dir == 0);
    CHECK(XDvdBackend_Stat("nope", &sz, &dir) == 0);
    CHECK(XDvdBackend_Stat("../x", &sz, &dir) == 0);
    unsigned char buf[6000]; unsigned got = 99;
    CHECK(XDvdBackend_Read("bio4/a.bin", 0, buf, 5000, &got) == 1 && got == 5000);
    int ok = 1; for (unsigned i = 0; i < 5000; i++) if (buf[i] != (unsigned char) (i * 3 + 1)) ok = 0; CHECK(ok);
    CHECK(XDvdBackend_Read("bio4/a.bin", 4990, buf, 100, &got) == 1 && got == 10 && buf[0] == (unsigned char) (4990 * 3 + 1));
    CHECK(XDvdBackend_Read("bio4/a.bin", 6000, buf, 100, &got) == 1 && got == 0);
    CHECK(XDvdBackend_Read("nope", 0, buf, 10, &got) == 0 && got == 0);
    // more files than the open-file cache holds, read in turns
    for (int r = 0; r < 3; r++) for (int i = 0; i < 6; i++) {
        char n[32]; snprintf(n, sizeof n, "m%d.bin", i);
        CHECK(XDvdBackend_Read(n, 10, buf, 20, &got) == 1 && got == 20 && buf[0] == (unsigned char) (10 * 3 + i));
    }
    // the whole layer on top
    XDvd_Reset(); XDvdBackend_Init(); XDvdBackend_SetRoot(root);
    DVDFileInfo fi; memset(&fi, 0, sizeof fi);
    CHECK(DVDOpen("/bio4\\a.bin", &fi) == TRUE);   // leading slash and backslash
    CHECK(fi.length == 5000);
    alignas(32) static unsigned char dst[5024];
    static int done; done = 0;
    DVDReadAsyncPrio(&fi, dst, 5024, 0, [](s32 r, DVDFileInfo*) { done = (int) r; }, 2);
    XDvd_Pump();
    CHECK(done == 5024 && dst[4999] == (unsigned char) (4999 * 3 + 1) && dst[5000] == 0 && dst[5023] == 0);
    (void) argc; (void) argv;
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); if (system(cmd)) {}
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
