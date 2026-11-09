#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>

#include "speech.h"

static HANDLE log_file = INVALID_HANDLE_VALUE;
static const char *log_tag = "";

void log_init(const std::wstring &dir, const char *tag)
{
    log_tag = tag;
    // Kept across reloads; started afresh only once it passes 20 MB, the old
    // one kept as .old. Opened for appending and shared, so the launcher and
    // the game can both write to it.
    std::wstring path = dir + L"\\FoG2Access.log";
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 20u << 20))
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_close()
{
    if (log_file != INVALID_HANDLE_VALUE)
        CloseHandle(log_file);
    log_file = INVALID_HANDLE_VALUE;
}

void logf(const char *fmt, ...)
{
    if (log_file == INVALID_HANDLE_VALUE)
        return;
    char buf[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int stamp = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d %s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                         log_tag);
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + stamp, sizeof buf - 2 - stamp, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    n += stamp;
    if (n > (int)sizeof buf - 3)
        n = sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    // One WriteFile per line: appends are atomic, so lines from the two
    // programs never interleave mid-line.
    DWORD written;
    WriteFile(log_file, buf, n, &written, nullptr);
}

std::wstring from_utf8(const std::string &s)
{
    if (s.empty())
        return {};
    // AMS strings are UTF-8; fall back to the ANSI code page if they aren't.
    UINT cp = CP_UTF8;
    int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    }
    std::wstring out(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string to_utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

typedef unsigned long (__stdcall *SpeakText_t)(const wchar_t *);
typedef unsigned long (__stdcall *NoArgs_t)(void);
static HMODULE nvda;
static SpeakText_t nvda_speak;
static NoArgs_t nvda_cancel;

void speech_close()
{
    nvda_speak = nullptr;
    nvda_cancel = nullptr;
    if (nvda)
        FreeLibrary(nvda);
    nvda = nullptr;
}

void speech_init(const std::wstring &dir)
{
    nvda = LoadLibraryW((dir + L"\\nvdaControllerClient32.dll").c_str());
    if (!nvda) {
        logf("nvdaControllerClient32.dll not found next to the game; no speech");
        return;
    }
    nvda_speak = (SpeakText_t)GetProcAddress(nvda, "nvdaController_speakText");
    nvda_cancel = (NoArgs_t)GetProcAddress(nvda, "nvdaController_cancelSpeech");
    NoArgs_t test = (NoArgs_t)GetProcAddress(nvda, "nvdaController_testIfRunning");
    logf("NVDA client loaded; NVDA %s", test && test() == 0 ? "is running" : "is not running");
}

void speak(const std::wstring &text, bool interrupt)
{
    logf("speak: %s", to_utf8(text).c_str());
    if (!nvda_speak)
        return;
    if (interrupt && nvda_cancel)
        nvda_cancel();
    nvda_speak(text.c_str());
}
