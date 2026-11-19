// Smoke test of the platform layers on the console: initialises them and prints what they report on screen.
#include <hal/debug.h>
#include <hal/video.h>
#include <stdio.h>
#include <windows.h>

#include <dolphin/os.h>
#include "xbox/xtrace.h"

#undef main
extern "C++" int re4_main();
int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
#ifndef RE4_DEBUGSCREEN
    debugPrint("RE4 / original Xbox: platform layers\n");
#endif
    XTrace_Add("RE4 / original Xbox: platform layers\n");
    OSInit();
#ifndef RE4_DEBUGSCREEN
    debugPrint("OSInit ok, arena %p .. %p\n", OSGetArenaLo(), OSGetArenaHi());
#endif
    { char b[80]; snprintf(b, sizeof(b), "OSInit ok, arena %p .. %p\n", OSGetArenaLo(), OSGetArenaHi()); XTrace_Add(b); }
#ifdef RE4_RUN_GAME
    XCrash_Install();
    XTrace_Add("calling re4_main\n");
    re4_main();
    XTrace_Add("re4_main returned\n");   // make RE4_RUN_GAME=1 (see docs/xbox.md): runs the game; not verified to boot
#endif
    for (;;) {
        Sleep(1000);
    }
    return 0;
}
