// The mod's entry points, called by the loader (src/loader) in both programs
// it runs in: the launcher (autorun.exe) and the game (FIELDOFGLORY2.EXE).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

#include "launcher.h"
#include "menus.h"
#include "speech.h"

static bool in_launcher;

extern "C" __declspec(dllexport) void mod_start(const wchar_t *dir)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t *name = wcsrchr(exe, L'\\');
    in_launcher = lstrcmpiW(name ? name + 1 : exe, L"autorun.exe") == 0;
    log_init(dir, in_launcher ? "launcher" : "game");
    logf("mod started");
    speech_init(dir);
    if (in_launcher)
        launcher_start();
    else
        menus_start(dir);
}

extern "C" __declspec(dllexport) void mod_stop(void)
{
    if (in_launcher)
        launcher_stop();
    else
        menus_stop();
    logf("mod stopped");
    speech_close();
    log_close();
}
