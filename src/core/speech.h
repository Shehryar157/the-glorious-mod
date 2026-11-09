// Speech through NVDA's controller client, and a log file next to the game.
#pragma once
#include <string>

// tag names the program in each log line ("launcher" or "game"): both write
// to the same log.
void log_init(const std::wstring &dir, const char *tag);
void log_close();
void logf(const char *fmt, ...);

void speech_init(const std::wstring &dir);
void speech_close();
// Speak text; interrupt cuts off whatever NVDA is saying first.
void speak(const std::wstring &text, bool interrupt = true);

std::wstring from_utf8(const std::string &s);
std::string to_utf8(const std::wstring &s);
