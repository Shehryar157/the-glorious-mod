// The mod's own words in the game's language (user, 2026-11-15: "the whole thing" in the language the
// player chose). Like Python's gettext: tr(L"selected") gives "ausgewählt" when the game is in German,
// the English text itself when a language or phrase is missing.
// Everything the game itself says is read from the game, already translated; this is only for what
// the mod adds ("selected", "out of command range", the key help...).
#pragma once
#include <initializer_list>
#include <string>

namespace lang {

enum Lang { EN, FR, DE, ES };

// The game's current language: in the game from its loaded strings (IDS_CONFIRM_YES reads Yes / Oui /
// Ja / Si), in the launcher from OPTIONS.TXT (Lang = "ENG", "FRE", "GER" or "SPA" as 4 bytes).
Lang current();

// The phrase in the current language; `en` is the key (the English text).
const wchar_t *tr(const wchar_t *en);

// tr() with {0}, {1}... filled in: trf(L"lost {0} men, {1} left", {L"12", L"300"}).
std::wstring trf(const wchar_t *en, std::initializer_list<std::wstring> args);

// The key a mouse click stands for ("Enter", "Entrée"...), and "<click>ing" ("pressing Enter").
const wchar_t *key_word();
const wchar_t *pressing_key();

// Tutorial order tooltips tell a mouse player which icon to click ("click on the Shoot icon",
// "auf das Symbol 'Schießen'", "sur l'icone Tirer", "en el icono Disparar"). Finds that phrase in
// `text`: the order's name and where the phrase starts and ends. False if there is none.
bool click_instruction(const std::wstring &text, std::wstring &name, size_t &from, size_t &to);

} // namespace lang

using lang::tr;
using lang::trf;
