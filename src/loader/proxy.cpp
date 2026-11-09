// Loader: a proxy version.dll. Both autorun.exe (the launcher) and
// FIELDOFGLORY2.EXE import version.dll, and Windows looks in the game's
// folder before System32, so both load us. We forward every version.dll
// function to the real DLL, then load the mod (FoG2Access.dll) and reload it
// whenever a new build lands, so neither program needs restarting after a
// change. This file should rarely change: changing it needs a restart.
//
// The loader also does the two things that must happen before the program's
// own code runs, which a reloaded mod would be too late for:
//   - remember the main (UI) thread: DllMain runs on it;
//   - in the launcher, catch the Lua state AutoPlay Media Studio creates at
//     startup (its luaL_newstate import is redirected through us).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static HMODULE real;
static wchar_t dir[MAX_PATH]; // the game folder (where this DLL is)
static const wchar_t *exe_tag; // "launcher" or "game": names the live copy and log lines
static DWORD main_thread;

static FARPROC real_proc(const char *name)
{
    if (!real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        lstrcpyW(path + n, L"\\version.dll");
        real = LoadLibraryW(path);
    }
    return real ? GetProcAddress(real, name) : nullptr;
}

// Each proxy looks the real function up once and calls it.
#define PROXY(ret, name, params, args, fail)                         \
    extern "C" ret WINAPI Proxy_##name params                        \
    {                                                                \
        typedef ret(WINAPI * fn_t) params;                           \
        static fn_t fn = (fn_t)real_proc(#name);                     \
        return fn ? fn args : fail;                                  \
    }

PROXY(BOOL, GetFileVersionInfoA, (LPCSTR f, DWORD h, DWORD n, LPVOID d), (f, h, n, d), FALSE)
PROXY(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD n, LPVOID d), (f, h, n, d), FALSE)
PROXY(BOOL, GetFileVersionInfoExA, (DWORD fl, LPCSTR f, DWORD h, DWORD n, LPVOID d), (fl, f, h, n, d), FALSE)
PROXY(BOOL, GetFileVersionInfoExW, (DWORD fl, LPCWSTR f, DWORD h, DWORD n, LPVOID d), (fl, f, h, n, d), FALSE)
PROXY(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeExA, (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h), 0)
PROXY(DWORD, GetFileVersionInfoSizeExW, (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h), 0)
PROXY(DWORD, VerFindFileA, (DWORD fl, LPCSTR f, LPCSTR w, LPCSTR a, LPSTR c, PUINT cn, LPSTR d, PUINT dn),
      (fl, f, w, a, c, cn, d, dn), 0)
PROXY(DWORD, VerFindFileW, (DWORD fl, LPCWSTR f, LPCWSTR w, LPCWSTR a, LPWSTR c, PUINT cn, LPWSTR d, PUINT dn),
      (fl, f, w, a, c, cn, d, dn), 0)
PROXY(DWORD, VerInstallFileA, (DWORD fl, LPCSTR s, LPCSTR d, LPCSTR sd, LPCSTR dd, LPCSTR cd, LPSTR t, PUINT tn),
      (fl, s, d, sd, dd, cd, t, tn), 0)
PROXY(DWORD, VerInstallFileW,
      (DWORD fl, LPCWSTR s, LPCWSTR d, LPCWSTR sd, LPCWSTR dd, LPCWSTR cd, LPWSTR t, PUINT tn),
      (fl, s, d, sd, dd, cd, t, tn), 0)
PROXY(BOOL, VerQueryValueA, (LPCVOID b, LPCSTR s, LPVOID *p, PUINT n), (b, s, p, n), FALSE)
PROXY(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID *p, PUINT n), (b, s, p, n), FALSE)

// The real version.dll forwards these two to kernel32.
extern "C" DWORD WINAPI Proxy_VerLanguageNameA(DWORD lang, LPSTR s, DWORD n)
{
    return VerLanguageNameA(lang, s, n);
}

extern "C" DWORD WINAPI Proxy_VerLanguageNameW(DWORD lang, LPWSTR s, DWORD n)
{
    return VerLanguageNameW(lang, s, n);
}

static void log_line(const char *text)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\FoG2Access_loader.log", dir);
    FILE *f = _wfopen(path, L"a");
    if (!f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d %ls %s\n", t.wHour, t.wMinute, t.wSecond, exe_tag, text);
    fclose(f);
}

// --- Lua state capture (launcher only) ---

typedef void *(__cdecl *NewState_t)(void);
static NewState_t real_newstate;
static void *volatile lua_state; // the first state AMS creates: its main one

static void log_line(const char *text);

static void *__cdecl hook_newstate(void)
{
    void *L = real_newstate();
    if (L && !lua_state) {
        lua_state = L;
        log_line("caught the Lua state from luaL_newstate");
    }
    return L;
}

// Backup: every AMS script call goes through lua_pcall with the state.
typedef int(__cdecl *PCall_t)(void *L, int nargs, int nresults, int errfunc);
static PCall_t real_pcall;

static int __cdecl hook_pcall(void *L, int nargs, int nresults, int errfunc)
{
    if (L && !lua_state) {
        lua_state = L;
        log_line("caught the Lua state from lua_pcall");
    }
    return real_pcall(L, nargs, nresults, errfunc);
}

// The import address table slot through which `module` calls dll!func.
static void **find_import(HMODULE module, const char *dll, const char *func)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY &imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress)
        return nullptr;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + imports.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), dll))
            continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            if (!lstrcmpA((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, func))
                return (void **)&slots->u1.Function;
        }
    }
    return nullptr;
}

// Point the program's import of lua5.1.dll!func at hook; returns the real function.
static void *hook_import(const char *func, void *hook)
{
    void **slot = find_import(GetModuleHandleW(nullptr), "lua5.1.dll", func);
    HMODULE lua = GetModuleHandleW(L"lua5.1.dll");
    void *expected = lua ? (void *)GetProcAddress(lua, func) : nullptr;
    char line[160];
    // Windows binds the program's imports before running DLL initialisers,
    // so the slot already holds the real address; check rather than assume.
    if (!slot || !expected || *slot != expected) {
        snprintf(line, sizeof line, "could not hook %s (import not found or not bound yet)", func);
        log_line(line);
        return nullptr;
    }
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    snprintf(line, sizeof line, "hooked %s", func);
    log_line(line);
    return expected;
}

static void hook_lua(void)
{
    real_newstate = (NewState_t)hook_import("luaL_newstate", (void *)hook_newstate);
    real_pcall = (PCall_t)hook_import("lua_pcall", (void *)hook_pcall);
}

// For the mod: the launcher's Lua state (null in the game), and the UI thread.
extern "C" void *loader_lua_state(void)
{
    return lua_state;
}

extern "C" DWORD loader_main_thread(void)
{
    return main_thread;
}

// --- mod loading ---

typedef void (*Start_t)(const wchar_t *dir);
typedef void (*Stop_t)(void);

// Live copies left by programs that have exited (one still in use is locked
// and stays).
static void delete_old_copies(void)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\FoG2Access.*.live.dll", dir);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        swprintf(path, MAX_PATH, L"%ls\\%ls", dir, found.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(h, &found));
    FindClose(h);
}

// Watch FoG2Access.dll; on each new build stop the running copy, load the new
// one and start it. We load a copy (FoG2Access.<tag>.<process id>.live.dll:
// the launcher and the game, or two games, can run at once) so the build can
// overwrite the original while it's in use.
static DWORD WINAPI watch(void *)
{
    wchar_t mod_path[MAX_PATH], live_path[MAX_PATH];
    swprintf(mod_path, MAX_PATH, L"%ls\\FoG2Access.dll", dir);
    delete_old_copies();
    swprintf(live_path, MAX_PATH, L"%ls\\FoG2Access.%ls.%lu.live.dll", dir, exe_tag, GetCurrentProcessId());
    log_line("loader started");
    HMODULE mod = nullptr;
    Stop_t stop = nullptr;
    FILETIME loaded = {};
    for (;; Sleep(250)) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(mod_path, GetFileExInfoStandard, &info))
            continue;
        if (CompareFileTime(&info.ftLastWriteTime, &loaded) == 0)
            continue;
        if (mod) {
            if (stop)
                stop();
            FreeLibrary(mod);
            mod = nullptr;
            stop = nullptr;
            log_line("stopped the old mod");
        }
        if (!CopyFileW(mod_path, live_path, FALSE))
            continue; // still being written; try again next time
        loaded = info.ftLastWriteTime;
        mod = LoadLibraryW(live_path);
        if (!mod) {
            log_line("could not load the live copy of FoG2Access.dll");
            continue;
        }
        Start_t start = (Start_t)GetProcAddress(mod, "mod_start");
        stop = (Stop_t)GetProcAddress(mod, "mod_stop");
        if (!start || !stop) {
            log_line("FoG2Access.dll lacks mod_start/mod_stop");
            continue;
        }
        start(dir);
        log_line("started the new mod");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        main_thread = GetCurrentThreadId();
        GetModuleFileNameW(inst, dir, MAX_PATH);
        for (int i = lstrlenW(dir) - 1; i >= 0; i--)
            if (dir[i] == L'\\') { dir[i] = 0; break; }
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t *name = exe;
        for (const wchar_t *p = exe; *p; p++)
            if (*p == L'\\')
                name = p + 1;
        exe_tag = lstrcmpiW(name, L"autorun.exe") == 0 ? L"launcher" : L"game";
        if (exe_tag[0] == L'l')
            hook_lua();
        // Windows holds the loader lock during DllMain, so loading the mod
        // waits for a thread that runs once it's released.
        HANDLE t = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
