// The Glorious Mod installer: installs, updates, reinstalls and removes the screen reader mods
// for the Field of Glory games. Plain Win32 controls (list box, edits, buttons, progress bar), so
// every screen reader reads them without help. The mod files are inside this exe (see readPayload);
// Install and Update fetch the newest GitHub release whose tag starts with the game's prefix
// (e.g. "fog2-v0.2.0") and take the files out of that release's TheGloriousMod.exe.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#define REPO_PATH L"Shehryar157/the-glorious-mod"
static const wchar_t* TITLE = L"The Glorious Mod installer";
static const wchar_t* REG_KEY = L"Software\\TheGloriousMod";

struct Game {
    const wchar_t* name;
    const char* tagPrefix;             // release tags for this game start with this
    const wchar_t* key;                // registry value name for the remembered folder
    const wchar_t* steamDir;           // folder under steamapps\common
    const wchar_t* exe;                // proves a folder is the game's
    const wchar_t* marker;             // our marker file in the game folder
    std::vector<std::wstring> files;   // downloaded from the release
    std::vector<std::wstring> extra;   // also removed on uninstall
    std::vector<std::wstring> procs;   // must be closed while we work
    bool available;
};

static std::vector<Game> games = {
    {L"Field of Glory II", "fog2-", L"fog2", L"Field of Glory II", L"FIELDOFGLORY2.EXE", L"FoG2Access.txt",
     {L"version.dll", L"FoG2Access.dll", L"nvdaControllerClient32.dll"},
     {L"FoG2Access.log", L"FoG2Access.log.old", L"FoG2Access_loader.log", L"FoG2Access.dll.tmp"},
     {L"fieldofglory2.exe", L"autorun.exe"}, true},
    {L"Field of Glory II: Medieval", "fog2m-", L"fog2m", L"", L"", L"", {}, {}, {}, false},
    {L"Field of Glory: Empires", "foge-", L"foge", L"", L"", L"", {}, {}, {}, false},
    {L"Field of Glory: Kingdoms", "fogk-", L"fogk", L"", L"", L"", {}, {}, {}, false},
};

enum { IDC_LIST = 101, IDC_STATUS, IDC_PATH, IDC_BROWSE, IDC_INSTALL, IDC_UPDATE, IDC_UNINSTALL, IDC_PROGRESS, IDC_LOG };
enum { WM_LOG = WM_APP + 1, WM_PROG, WM_DONE };
enum Action { INSTALL, UPDATE, UNINSTALL };

static HWND hwnd, hList, hStatus, hPath, hBrowse, hInstall, hUpdate, hUninstall, hProgress, hLog, hClose, lastFocus;
static bool busy;
static std::wstring paths[4];

// ---------- small helpers ----------

static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool readFile(const std::wstring& p, std::string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    bool ok = size == 0 || (ReadFile(f, &out[0], size, &got, nullptr) && got == size);
    CloseHandle(f);
    return ok;
}

static bool writeFile(const std::wstring& p, const std::string& data) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    CloseHandle(f);
    return ok;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring errorText(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    if (code >= 12000 && code < 12200 && winhttp) flags = (flags & ~FORMAT_MESSAGE_FROM_SYSTEM) | FORMAT_MESSAGE_FROM_HMODULE;
    std::wstring s;
    if (FormatMessageW(flags, winhttp, code, 0, (LPWSTR)&buf, 0, nullptr) && buf) {
        s = buf;
        LocalFree(buf);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    } else {
        s = L"error " + std::to_wstring(code);
    }
    return s;
}

static std::wstring regRead(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

static void regWrite(const wchar_t* value, const std::wstring& data) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

// ---------- finding the game ----------

static bool isGameFolder(const Game& g, const std::wstring& dir) {
    return !dir.empty() && fileExists(dir + L"\\" + g.exe);
}

// Steam keeps its libraries in steamapps\libraryfolders.vdf as  "path"  "D:\\steam"  lines.
static std::wstring findSteamGame(const Game& g) {
    std::vector<std::wstring> bases, libs;
    std::wstring s = regRead(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (!s.empty()) bases.push_back(s);
    s = regRead(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!s.empty()) bases.push_back(s);
    for (auto& base : bases) {
        libs.push_back(base);
        std::string vdf;
        if (!readFile(base + L"\\steamapps\\libraryfolders.vdf", vdf)) continue;
        for (size_t at = vdf.find("\"path\""); at != std::string::npos; at = vdf.find("\"path\"", at + 6)) {
            size_t open = vdf.find('"', at + 6), close = open == std::string::npos ? open : vdf.find('"', open + 1);
            while (close != std::string::npos && vdf[close - 1] == '\\' && vdf[close - 2] != '\\') close = vdf.find('"', close + 1);
            if (close == std::string::npos) break;
            std::string raw = vdf.substr(open + 1, close - open - 1), path;
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') i++;
                path += raw[i];
            }
            libs.push_back(widen(path));
        }
    }
    for (auto& lib : libs) {
        std::wstring dir = lib + L"\\steamapps\\common\\" + g.steamDir;
        if (isGameFolder(g, dir)) return dir;
    }
    return L"";
}

static std::wstring gamePath(int i) {
    const Game& g = games[i];
    if (paths[i].empty()) {
        std::wstring saved = regRead(HKEY_CURRENT_USER, REG_KEY, g.key);
        paths[i] = isGameFolder(g, saved) ? saved : findSteamGame(g);
    }
    return paths[i];
}

// "" = not installed. The marker holds "version=<tag>"; a marker without one came from build.py.
static std::wstring installedVersion(const Game& g, const std::wstring& dir) {
    std::string m;
    if (!readFile(dir + L"\\" + g.marker, m)) return L"";
    size_t at = m.find("version=");
    if (at == std::string::npos) return L"development build";
    size_t end = m.find_first_of("\r\n", at);
    return widen(m.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8));
}

static bool anyRunning(const Game& g) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        for (auto& p : g.procs)
            if (_wcsicmp(pe.szExeFile, p.c_str()) == 0) found = true;
    CloseHandle(snap);
    return found;
}

// ---------- talking to the UI from the worker ----------

static void logLine(const std::wstring& s) { PostMessageW(hwnd, WM_LOG, 0, (LPARAM) new std::wstring(s)); }
static void progress(int permille) { PostMessageW(hwnd, WM_PROG, permille, 0); }

// ---------- downloading ----------

static bool httpGet(const std::wstring& url, bool api, std::string& out,
                    const std::function<void(unsigned long long, unsigned long long)>& onProgress, std::wstring& err) {
    wchar_t host[256], path[4096], extra[4096];
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"bad address " + url; return false; }
    std::wstring object = std::wstring(path) + extra;
    out.clear();
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"TheGloriousMod-Installer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (r && WinHttpSendRequest(r, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        sz = sizeof len;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            err = L"the server answered " + std::to_wstring(status);
        } else {
            ok = true;
            char buf[65536];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &got)) { ok = false; err = errorText(GetLastError()); break; }
                if (got == 0) break;
                out.append(buf, got);
                if (onProgress) onProgress(out.size(), len);
            }
            if (ok && len && out.size() != len) { ok = false; err = L"the download was cut short"; }
        }
    } else {
        err = errorText(GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

// Tiny JSON reading, enough for GitHub's release list: the raw value after "key": in [from, to).
static std::string jsonValue(const std::string& j, size_t from, size_t to, const char* key, size_t* where = nullptr) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = j.find(k, from);
    if (at == std::string::npos || at >= to) return "";
    if (where) *where = at;
    size_t p = at + k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == ':' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) p++;
    std::string v;
    if (p < j.size() && j[p] == '"') {
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) p++;
            v += j[p];
        }
    } else {
        while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']' && j[p] != ' ' && j[p] != '\n') v += j[p++];
    }
    return v;
}

struct Asset { std::wstring name, url; unsigned long long size; };

// Releases come newest first. Each release's fields run from its "tag_name" to the next one's;
// its assets (with "size" before "browser_download_url") sit inside that stretch.
static bool newestRelease(const std::string& j, const char* prefix, std::wstring& tag, std::vector<Asset>& assets) {
    std::vector<size_t> starts;
    for (size_t at = j.find("\"tag_name\""); at != std::string::npos; at = j.find("\"tag_name\"", at + 10)) starts.push_back(at);
    for (size_t i = 0; i < starts.size(); i++) {
        size_t from = starts[i], to = i + 1 < starts.size() ? starts[i + 1] : j.size();
        std::string t = jsonValue(j, from, to, "tag_name");
        if (t.compare(0, strlen(prefix), prefix) != 0) continue;
        if (jsonValue(j, from, to, "draft") == "true" || jsonValue(j, from, to, "prerelease") == "true") continue;
        tag = widen(t);
        assets.clear();
        size_t at = from;
        for (;;) {
            size_t urlAt;
            std::string url = jsonValue(j, at, to, "browser_download_url", &urlAt);
            if (url.empty()) break;
            size_t sizeAt = j.rfind("\"size\"", urlAt);
            unsigned long long size = sizeAt != std::string::npos && sizeAt > at ? strtoull(jsonValue(j, sizeAt, to, "size").c_str(), nullptr, 10) : 0;
            assets.push_back({widen(url.substr(url.rfind('/') + 1)), widen(url), size});
            at = urlAt + 22;
        }
        return true;
    }
    return false;
}

// ---------- the jobs ----------

static bool replaceFile(const std::wstring& dest, const std::string& data, std::wstring& msg) {
    std::wstring tmp = dest + L".new";
    if (!writeFile(tmp, data) || !MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        msg = L"Couldn't write " + dest + L": " + errorText(e) + L".";
        if (e == ERROR_ACCESS_DENIED) msg += L" If the game is under Program Files, run this installer as administrator.";
        return false;
    }
    return true;
}

static bool doUninstall(const Game& g, const std::wstring& dir, std::wstring& msg) {
    if (!fileExists(dir + L"\\" + g.marker)) { msg = L"The mod isn't installed for " + std::wstring(g.name) + L"."; return false; }
    std::vector<std::wstring> names = g.files;
    names.insert(names.end(), g.extra.begin(), g.extra.end());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\FoG2Access.*.live.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    names.push_back(g.marker);  // last, so a failed run can be retried
    for (size_t i = 0; i < names.size(); i++) {
        std::wstring p = dir + L"\\" + names[i];
        if (fileExists(p)) {
            if (!DeleteFileW(p.c_str())) { msg = L"Couldn't remove " + p + L": " + errorText(GetLastError()) + L"."; return false; }
            logLine(L"Removed " + names[i]);
        }
        progress((int)(1000 * (i + 1) / names.size()));
    }
    msg = L"Removed The Glorious Mod from " + std::wstring(g.name) + L".";
    return true;
}

// The mod files travel inside TheGloriousMod.exe (user, 2026-11-09: people download that one file only),
// appended after the program by build.py: entries {uint32 name length, name ("fog2/version.dll", and
// "fog2/version.txt" = the release tag), uint32 size, data}, then a footer {uint32 offset of the first entry,
// "GLORIOUSMOD1"}. Install / Update download the newest release's TheGloriousMod.exe and take the files from
// it; without GitHub, Install uses the copy inside the installer that is running.
static const char MAGIC[] = "GLORIOUSMOD1";

static bool readPayload(const std::string& exe, std::map<std::string, std::string>& files) {
    files.clear();
    if (exe.size() < 16 || exe.compare(exe.size() - 12, 12, MAGIC, 12) != 0) return false;
    uint32_t at;
    memcpy(&at, exe.data() + exe.size() - 16, 4);
    size_t end = exe.size() - 16;
    while (at + 8 <= end) {
        uint32_t len, size;
        memcpy(&len, exe.data() + at, 4);
        if (at + 4 + len + 4 > end) return false;
        std::string name = exe.substr(at + 4, len);
        memcpy(&size, exe.data() + at + 4 + len, 4);
        size_t data = at + 8 + len;
        if (data + size > end) return false;
        files[name] = exe.substr(data, size);
        at = (uint32_t)(data + size);
    }
    return !files.empty();
}

static bool ownPayload(std::map<std::string, std::string>& files) {
    wchar_t path[MAX_PATH];
    std::string exe;
    return GetModuleFileNameW(nullptr, path, MAX_PATH) && readFile(path, exe) && readPayload(exe, files);
}

static bool doInstall(const Game& g, const std::wstring& dir, Action action, std::wstring& msg) {
    std::wstring before = installedVersion(g, dir);
    if (fileExists(dir + L"\\version.dll") && before.empty()) {
        msg = L"The game folder already has a version.dll from another mod or program; the installer won't overwrite it.";
        return false;
    }
    std::string key = narrow(g.key) + "/";
    std::map<std::string, std::string> own, files;
    ownPayload(own);
    std::wstring ownTag = own.count(key + "version.txt") ? widen(own[key + "version.txt"]) : L"";

    logLine(L"Asking GitHub for the newest release.");
    progress(10);
    std::string json;
    std::wstring err, tag;
    std::vector<Asset> assets;
    bool online = httpGet(L"https://api.github.com/repos/" REPO_PATH L"/releases?per_page=50", true, json, nullptr, err);
    if (online && newestRelease(json, g.tagPrefix, tag, assets)) {
        logLine(L"Newest release: " + tag);
        progress(50);
        if (action == UPDATE && before == tag) { progress(1000); msg = L"Already up to date: " + tag + L"."; return true; }
        if (tag == ownTag) {
            files = own; // this installer already holds that release
        } else {
            const Asset* a = nullptr;
            for (auto& x : assets) if (_wcsicmp(x.name.c_str(), L"TheGloriousMod.exe") == 0) a = &x;
            if (!a) { msg = L"Release " + tag + L" has no TheGloriousMod.exe."; return false; }
            logLine(L"Downloading " + tag);
            std::string exe;
            unsigned long long total = a->size;
            auto onProgress = [&](unsigned long long got, unsigned long long len) {
                unsigned long long all = len ? len : total;
                if (all) progress(50 + (int)(900 * got / all));
            };
            if (!httpGet(a->url, false, exe, onProgress, err)) { msg = L"Downloading " + tag + L" failed: " + err + L"."; return false; }
            if (!readPayload(exe, files)) { msg = L"The download of " + tag + L" holds no mod files."; return false; }
        }
    } else if (action == UPDATE) {
        msg = online ? L"There's no release for " + std::wstring(g.name) + L" on GitHub yet."
                     : L"Couldn't reach GitHub to look for an update: " + err + L".";
        return false;
    } else if (!ownTag.empty()) {
        logLine(online ? L"No release on GitHub; installing the copy inside this installer."
                       : L"Couldn't reach GitHub (" + err + L"); installing the copy inside this installer.");
        files = own;
        tag = ownTag;
    } else {
        msg = L"Couldn't reach GitHub: " + err + L".";
        return false;
    }
    if (files.count(key + "version.txt")) tag = widen(files[key + "version.txt"]);
    std::vector<std::string> data;
    for (auto& f : g.files) {
        auto it = files.find(key + narrow(f));
        if (it == files.end()) { msg = L"Release " + tag + L" is missing " + f + L"."; return false; }
        data.push_back(it->second);
    }
    progress(950);
    logLine(L"Copying the files into the game folder.");
    for (size_t i = 0; i < data.size(); i++)
        if (!replaceFile(dir + L"\\" + g.files[i], data[i], msg)) return false;
    if (!replaceFile(dir + L"\\" + g.marker, "The Glorious Mod installed this version.dll\r\nversion=" + narrow(tag) + "\r\n", msg)) return false;
    progress(1000);
    if (before.empty()) msg = L"Installed The Glorious Mod " + tag + L" for " + g.name + L".";
    else if (before == tag) msg = L"Reinstalled " + tag + L".";
    else msg = L"Updated from " + before + L" to " + tag + L".";
    return true;
}

struct Job { Action action; int game; std::wstring dir; };

static DWORD WINAPI worker(void* p) {
    Job job = *(Job*)p;
    delete (Job*)p;
    const Game& g = games[job.game];
    std::wstring msg;
    bool ok;
    if (anyRunning(g)) {
        ok = false;
        msg = L"Close " + std::wstring(g.name) + L" and its launcher first, then try again.";
    } else {
        ok = job.action == UNINSTALL ? doUninstall(g, job.dir, msg) : doInstall(g, job.dir, job.action, msg);
    }
    PostMessageW(hwnd, WM_DONE, ok, (LPARAM) new std::wstring(msg));
    return 0;
}

// ---------- the window ----------

static int selected() {
    int i = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : i;
}

static void refresh() {
    int i = selected();
    const Game& g = games[i];
    BOOL install = FALSE, installed = FALSE, browse = g.available;
    std::wstring status, dir;
    if (!g.available) {
        status = L"Not available yet.";
    } else {
        dir = gamePath(i);
        if (!isGameFolder(g, dir)) {
            status = L"Game not found. Use Browse to pick its folder.";
            dir = L"";
        } else {
            std::wstring v = installedVersion(g, dir);
            status = v.empty() ? L"Not installed." : L"Installed: " + v + L".";
            install = TRUE;
            installed = !v.empty();
        }
    }
    SetWindowTextW(hStatus, status.c_str());
    SetWindowTextW(hPath, dir.c_str());
    SetWindowTextW(hInstall, installed ? L"&Reinstall" : L"&Install");
    EnableWindow(hBrowse, browse);
    EnableWindow(hInstall, install);
    EnableWindow(hUpdate, installed);
    EnableWindow(hUninstall, installed);
}

static void appendLog(const std::wstring& s) {
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)(s + L"\r\n").c_str());
}

static void start(Action action) {
    int i = selected();
    if (action == UNINSTALL &&
        MessageBoxW(hwnd, (L"Remove The Glorious Mod from " + std::wstring(games[i].name) + L"?").c_str(), TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    busy = true;
    SetFocus(hLog);
    for (HWND h : {hList, hBrowse, hInstall, hUpdate, hUninstall, hClose}) EnableWindow(h, FALSE);
    SendMessageW(hProgress, PBM_SETPOS, 0, 0);
    const wchar_t* verb = action == UNINSTALL ? L"Uninstalling" : action == UPDATE ? L"Updating" : L"Installing";
    appendLog(std::wstring(verb) + L" for " + games[i].name + L".");
    CloseHandle(CreateThread(nullptr, 0, worker, new Job{action, i, gamePath(i)}, 0, nullptr));
}

static void browse() {
    int i = selected();
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle((L"Pick the " + std::wstring(games[i].name) + L" folder").c_str());
    IShellItem* item = nullptr;
    PWSTR p = nullptr;
    if (SUCCEEDED(dlg->Show(hwnd)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        std::wstring dir = p;
        CoTaskMemFree(p);
        if (isGameFolder(games[i], dir)) {
            paths[i] = dir;
            regWrite(games[i].key, dir);
            refresh();
        } else {
            MessageBoxW(hwnd, (L"That folder doesn't contain " + std::wstring(games[i].exe) + L".").c_str(), TITLE, MB_ICONWARNING);
        }
    }
    if (item) item->Release();
    dlg->Release();
}

static HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static BOOL CALLBACK setFont(HWND h, LPARAM font) {
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static void createControls() {
    // Each label comes right before its control, so screen readers use it as the control's name.
    add(L"STATIC", L"Which &game?", 0, 12, 10, 480, 20, -1);
    hList = add(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 12, 32, 480, 76, IDC_LIST, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"&Status", 0, 12, 116, 480, 20, -1);
    hStatus = add(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, 12, 136, 480, 24, IDC_STATUS, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"Game &folder", 0, 12, 168, 480, 20, -1);
    hPath = add(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, 12, 188, 380, 24, IDC_PATH, WS_EX_CLIENTEDGE);
    hBrowse = add(L"BUTTON", L"&Browse...", WS_TABSTOP, 400, 187, 92, 26, IDC_BROWSE);
    hInstall = add(L"BUTTON", L"&Install", WS_TABSTOP, 12, 226, 112, 30, IDC_INSTALL);
    hUpdate = add(L"BUTTON", L"&Update", WS_TABSTOP, 134, 226, 112, 30, IDC_UPDATE);
    hUninstall = add(L"BUTTON", L"U&ninstall", WS_TABSTOP, 256, 226, 112, 30, IDC_UNINSTALL);
    hClose = add(L"BUTTON", L"&Close", WS_TABSTOP, 380, 226, 112, 30, IDCANCEL);
    add(L"STATIC", L"Progress", 0, 12, 266, 480, 20, -1);
    hProgress = add(PROGRESS_CLASSW, L"Progress", 0, 12, 286, 480, 20, IDC_PROGRESS);
    SendMessageW(hProgress, PBM_SETRANGE32, 0, 1000);
    add(L"STATIC", L"&Log", 0, 12, 314, 480, 20, -1);
    hLog = add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL, 12, 334, 480, 120, IDC_LOG, WS_EX_CLIENTEDGE);

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    EnumChildWindows(hwnd, setFont, (LPARAM)font);

    for (auto& g : games) {
        std::wstring item = g.name;
        if (!g.available) item += L" (not available yet)";
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
    SendMessageW(hList, LB_SETCURSEL, 0, 0);
    refresh();
    SetFocus(hList);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        hwnd = h;
        createControls();
        return 0;
    case WM_ACTIVATE:  // keep focus where the user left it when switching windows
        if (LOWORD(wp) == WA_INACTIVE) lastFocus = GetFocus();
        else if (lastFocus && IsWindowEnabled(lastFocus)) SetFocus(lastFocus);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LIST: if (HIWORD(wp) == LBN_SELCHANGE) refresh(); break;
        case IDC_BROWSE: browse(); break;
        case IDC_INSTALL: start(INSTALL); break;
        case IDC_UPDATE: start(UPDATE); break;
        case IDC_UNINSTALL: start(UNINSTALL); break;
        case IDCANCEL: PostMessageW(h, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_LOG: {
        std::wstring* s = (std::wstring*)lp;
        appendLog(*s);
        delete s;
        return 0;
    }
    case WM_PROG:
        SendMessageW(hProgress, PBM_SETPOS, wp, 0);
        return 0;
    case WM_DONE: {
        std::wstring* s = (std::wstring*)lp;
        appendLog(*s);
        busy = false;
        EnableWindow(hList, TRUE);
        EnableWindow(hClose, TRUE);
        refresh();
        MessageBoxW(h, s->c_str(), TITLE, wp ? MB_ICONINFORMATION : MB_ICONERROR);
        delete s;
        SetFocus(hList);
        return 0;
    }
    case WM_CLOSE:
        if (busy) { MessageBeep(MB_ICONWARNING); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TheGloriousModInstaller";
    RegisterClassW(&wc);
    RECT r{0, 0, 504, 466};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
                    r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return 0;
}
