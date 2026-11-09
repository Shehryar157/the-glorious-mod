// The launcher (autorun.exe, an AutoPlay Media Studio 8 program).
//
// Its buttons are pictures AMS draws itself, not windows, so a screen reader
// can't see them and Tab can't reach them. AMS runs on Lua, and its Lua API
// knows every object on the page: Page.EnumerateObjects, Button.GetText,
// Page.ClickObject... The loader caught AMS's Lua state at startup; we run
// small Lua snippets on it to list the buttons and press them.
//
// Keys: a WH_GETMESSAGE hook on the launcher's UI thread sees each key as the
// launcher takes it from its queue, on that same thread, so calling into
// AMS's Lua there is safe, and a key we handle is turned into WM_NULL before
// any window sees it. Nothing polls.
//
// The list is the language box (a real combo box, which keeps its own keys and
// is read by NVDA) followed by the visible buttons in reading order.
//   Tab / Shift+Tab   next / previous item (wraps)
//   on a button: Up/Down/Left/Right move, Home/End first/last, letters jump
//   to the next item starting with that letter, Enter/Space press it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#include "launcher.h"
#include "speech.h"

// --- Lua (lua5.1.dll's C API, cdecl) ---

typedef int(__cdecl *LoadBuffer_t)(void *L, const char *buf, size_t size, const char *name);
typedef int(__cdecl *PCall_t)(void *L, int nargs, int nresults, int errfunc);
typedef const char *(__cdecl *ToLString_t)(void *L, int index, size_t *len);
typedef int(__cdecl *GetTop_t)(void *L);
typedef void(__cdecl *SetTop_t)(void *L, int index);

static LoadBuffer_t lua_loadbuffer;
static PCall_t lua_pcall;
static ToLString_t lua_tolstring;
static GetTop_t lua_gettop;
static SetTop_t lua_settop;
static void *(*loader_lua_state)(void);

// Run a chunk; true and its first result as a string, or false (error logged).
// UI thread only.
static bool run_lua(const std::string &code, std::string *result = nullptr)
{
    void *L = loader_lua_state ? loader_lua_state() : nullptr;
    if (!L || !lua_loadbuffer)
        return false;
    int top = lua_gettop(L);
    bool ok = lua_loadbuffer(L, code.data(), code.size(), "FoG2Access") == 0 && lua_pcall(L, 0, 1, 0) == 0;
    size_t len = 0;
    const char *s = lua_tolstring(L, -1, &len);
    if (!ok)
        logf("lua error: %s", s ? s : "?");
    else if (result)
        result->assign(s ? s : "", s ? len : 0);
    lua_settop(L, top);
    return ok;
}

static std::string lua_quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out + "\"";
}

// Every object on the page: name, kind, text, x, y, width, height, visible,
// enabled; fields split by \31, objects by \30. Kinds come from AMS's own
// OBJECT_* constants ("button", "label", ...).
static const char LIST_OBJECTS[] = R"lua(
local kinds = {}
for k, v in pairs(_G) do
  if type(k) == "string" and k:sub(1, 7) == "OBJECT_" and type(v) == "number" then kinds[v] = k:sub(8):lower() end
end
local api = {button = Button, label = Label, hotspot = Hotspot, image = Image, paragraph = Paragraph}
local out = {}
for _, n in ipairs(Page.EnumerateObjects() or {}) do
  local t = Page.GetObjectType(n)
  local kind = kinds[t] or tostring(t)
  local a = api[kind]
  local text, x, y, w, h, vis, en = "", 0, 0, 0, 0, true, true
  if a then
    pcall(function() if a.GetText then text = a.GetText(n) or "" end end)
    pcall(function() local p = a.GetPos(n); x, y = p.X, p.Y end)
    pcall(function() local s = a.GetSize(n); w, h = s.Width, s.Height end)
    pcall(function() vis = a.IsVisible(n) end)
    pcall(function() en = a.IsEnabled(n) end)
  end
  out[#out + 1] = table.concat({n, kind, text, x, y, w, h, tostring(vis), tostring(en)}, "\31")
end
return table.concat(out, "\30")
)lua";

// --- the item list ---

struct Item {
    std::string name; // AMS object name; empty for the language box
    std::wstring text;
    int x, y;
    bool enabled;
};

static std::vector<Item> items;
static size_t current;          // index into items
static std::string current_name; // keeps the place when the list is rebuilt
static HWND launcher_window, combo;
static bool logged_objects;

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

static void rebuild()
{
    std::string listing;
    items.clear();
    items.push_back({"", L"", 0, 0, true}); // the language box: top left, so first
    if (run_lua(LIST_OBJECTS, &listing) && !listing.empty()) {
        std::vector<Item> buttons;
        // Lua's "\30" and "\31" are decimal, C++'s are octal: use hex here.
        for (const std::string &object : split(listing, '\x1e')) {
            std::vector<std::string> f = split(object, '\x1f');
            if (f.size() < 9)
                continue;
            if (!logged_objects)
                logf("object %s: %s '%s' at %s,%s size %sx%s visible %s enabled %s", f[0].c_str(), f[1].c_str(),
                     f[2].c_str(), f[3].c_str(), f[4].c_str(), f[5].c_str(), f[6].c_str(), f[7].c_str(), f[8].c_str());
            if (f[1] != "button" || f[7] != "true")
                continue;
            std::wstring text = from_utf8(f[2]);
            buttons.push_back({f[0], text.empty() ? from_utf8(f[0]) : text, atoi(f[3].c_str()), atoi(f[4].c_str()),
                               f[8] == "true"});
        }
        logged_objects = true;
        // Reading order: rows top to bottom (buttons within 20 pixels of each
        // other's top count as one row), left to right within a row.
        std::sort(buttons.begin(), buttons.end(), [](const Item &a, const Item &b) {
            if (abs(a.y - b.y) > 20)
                return a.y < b.y;
            return a.x < b.x;
        });
        items.insert(items.end(), buttons.begin(), buttons.end());
    }
    current = 0;
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].name == current_name)
            current = i;
}

static void announce()
{
    const Item &item = items[current];
    std::wstring text;
    if (item.name.empty()) {
        wchar_t buf[256] = L"";
        GetWindowTextW(combo, buf, 256); // a drop-down list's text is its selection
        text = std::wstring(buf) + L", combo box";
    } else {
        text = item.text;
        if (!item.enabled)
            text += L", unavailable";
    }
    text += L", " + std::to_wstring(current + 1) + L" of " + std::to_wstring(items.size());
    speak(text);
}

static void move_to(size_t index)
{
    current = index;
    current_name = items[index].name;
    // The language box needs real keyboard focus to work. If it doesn't have
    // it yet, NVDA announces it when it gets it, so we don't.
    if (items[index].name.empty() && GetFocus() != combo) {
        SetFocus(combo);
        return;
    }
    announce();
}

// --- keys ---

static BOOL CALLBACK find_combo(HWND child, LPARAM out)
{
    wchar_t cls[32];
    if (GetClassNameW(child, cls, 32) && !lstrcmpiW(cls, L"ComboBox")) {
        *(HWND *)out = child;
        return FALSE;
    }
    return TRUE;
}

// Is this key for the launcher's main window (the one holding the language
// box)? Caches the window.
static bool in_launcher(HWND hwnd)
{
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root)
        return false;
    if (root == launcher_window && IsWindow(combo))
        return true;
    HWND found = nullptr;
    EnumChildWindows(root, find_combo, (LPARAM)&found);
    if (!found)
        return false;
    launcher_window = root;
    combo = found;
    return true;
}

enum Action { NONE, NEXT, PREV, TAB_NEXT, TAB_PREV, FIRST, LAST, PRESS, SEARCH, SWALLOW };

// What a key does, without doing it (the hook is also asked about keys that
// are only being peeked at).
static Action classify(WPARAM vk, bool on_combo)
{
    if (vk == VK_TAB)
        return GetKeyState(VK_SHIFT) < 0 ? TAB_PREV : TAB_NEXT;
    if (on_combo || GetKeyState(VK_CONTROL) < 0)
        return NONE; // the language box keeps its keys
    switch (vk) {
    case VK_DOWN: case VK_RIGHT: return NEXT;
    case VK_UP: case VK_LEFT: return PREV;
    case VK_HOME: return FIRST;
    case VK_END: return LAST;
    case VK_RETURN: case VK_SPACE: return PRESS;
    case VK_PRIOR: case VK_NEXT: return SWALLOW; // would change the language behind the user's back
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return SEARCH;
    return NONE;
}

static void act(Action action, WPARAM vk)
{
    size_t n = items.size();
    switch (action) {
    case NEXT: move_to(current + 1 < n ? current + 1 : current); break;
    case PREV: move_to(current > 0 ? current - 1 : 0); break;
    case TAB_NEXT: move_to((current + 1) % n); break;
    case TAB_PREV: move_to((current + n - 1) % n); break;
    case FIRST: move_to(0); break;
    case LAST: move_to(n - 1); break;
    case PRESS:
        logf("pressing %s", items[current].name.c_str());
        run_lua("Page.ClickObject(" + lua_quote(items[current].name) + ")");
        break;
    case SEARCH:
        for (size_t step = 1; step <= n; step++) {
            const Item &item = items[(current + step) % n];
            if (!item.text.empty() && towupper(item.text[0]) == (wchar_t)vk) {
                move_to((current + step) % n);
                break;
            }
        }
        break;
    default: break;
    }
}

static HHOOK hook;
static std::atomic<int> busy; // hook calls in progress, so stopping can wait them out

static LRESULT CALLBACK on_message(int code, WPARAM remove, LPARAM lp)
{
    busy++;
    MSG *msg = (MSG *)lp;
    if (code == HC_ACTION && msg->message == WM_KEYDOWN && in_launcher(msg->hwnd)) {
        if (items.empty())
            rebuild();
        Action action = classify(msg->wParam, items[current].name.empty());
        if (action != NONE) {
            if (remove == PM_REMOVE) {
                rebuild(); // cheap (a dozen objects), and picks up anything AMS showed or hid
                act(action, msg->wParam);
            }
            msg->message = WM_NULL;
        }
    }
    LRESULT r = CallNextHookEx(nullptr, code, remove, lp);
    busy--;
    return r;
}

void launcher_start()
{
    HMODULE loader = GetModuleHandleW(L"version.dll");
    loader_lua_state = loader ? (void *(*)(void))GetProcAddress(loader, "loader_lua_state") : nullptr;
    DWORD (*main_thread)(void) = loader ? (DWORD(*)(void))GetProcAddress(loader, "loader_main_thread") : nullptr;
    HMODULE lua = GetModuleHandleW(L"lua5.1.dll");
    if (lua) {
        lua_loadbuffer = (LoadBuffer_t)GetProcAddress(lua, "luaL_loadbuffer");
        lua_pcall = (PCall_t)GetProcAddress(lua, "lua_pcall");
        lua_tolstring = (ToLString_t)GetProcAddress(lua, "lua_tolstring");
        lua_gettop = (GetTop_t)GetProcAddress(lua, "lua_gettop");
        lua_settop = (SetTop_t)GetProcAddress(lua, "lua_settop");
    }
    void *L = loader_lua_state ? loader_lua_state() : nullptr;
    logf("launcher: loader %s, lua5.1 %s, Lua state %p", loader_lua_state ? "found" : "missing", lua ? "found" : "missing", L);
    if (!main_thread)
        return;
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&on_message, &self);
    hook = SetWindowsHookExW(WH_GETMESSAGE, on_message, self, main_thread());
    logf("launcher: key hook %s", hook ? "installed" : "failed");
}

void launcher_stop()
{
    if (hook)
        UnhookWindowsHookEx(hook);
    hook = nullptr;
    while (busy > 0)
        Sleep(1);
    items.clear();
    launcher_window = combo = nullptr;
    logged_objects = false;
}
