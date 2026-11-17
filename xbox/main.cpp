// Smoke test of the platform layers on the console: initialises them and prints what they report on screen.
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

#include <dolphin/os.h>

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("RE4 / original Xbox: platform layers\n");
    OSInit();
    debugPrint("OSInit ok, arena %p .. %p\n", OSGetArenaLo(), OSGetArenaHi());
    for (;;) {
        Sleep(1000);
    }
    return 0;
}
