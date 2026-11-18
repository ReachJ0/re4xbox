// Smoke test of the platform layers on the console: initialises them and prints what they report on screen.
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

#include <dolphin/os.h>

#undef main
extern "C++" int re4_main();
int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("RE4 / original Xbox: platform layers\n");
    OSInit();
    debugPrint("OSInit ok, arena %p .. %p\n", OSGetArenaLo(), OSGetArenaHi());
#ifdef RE4_RUN_GAME
    re4_main();   // make RE4_RUN_GAME=1 (see docs/xbox.md): runs the game; not verified to boot
#endif
    for (;;) {
        Sleep(1000);
    }
    return 0;
}
