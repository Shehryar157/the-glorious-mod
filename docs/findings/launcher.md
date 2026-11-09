# Launcher

## 2026-10-31 — what the language screen is (verified with UI Automation on the running window)

Steam starts `autorun.exe`, not the game. It is an **AutoPlay Media Studio 8** (Indigo Rose) menu
(window title "FoG2 Gamemenu", MFC; project in `AutoPlay/autorun.cdd`, an encrypted zip; Lua 5.1 scripting).
Its accessible tree: one Win32 `ComboBox` (language, shows "English") and an embedded Internet Explorer pane
(news page). The Play / Manual / Modding buttons are images drawn by AMS, so screen readers cannot see them.
`lang-xml/Language_File_*.xml` only translate the launcher's own three button labels.

The launcher's language setting only affects the launcher; the game has its own language selection.

## 2026-10-31 — layout and internals (verified: PrintWindow capture, PE imports, exe strings)

- Client area 900x550 (window is not DPI aware; Windows scales it to 1350x825 at 150%).
- Drawn buttons, with the text painted on them: Play Game, Game Manual, EULA, Store, Register, Catalog, Forums,
  Website, Patch Notes, X (close, top right). Two logo images (Slitherine, Byzantine Games) at the bottom. Language
  combo top left. Left panel is the IE news pane ("You may also like", news post, red links).
- autorun.exe: 32-bit, no ASLR, imports `VERSION.dll` and `WINMM.dll` (not KnownDLLs), so the **same
  version.dll proxy loads into the launcher and the game**.
- It imports 37 Lua 5.1 functions from `lua5.1.dll`, including luaL_newstate, lua_pcall, luaL_loadbuffer.
  The exe contains the AMS action names EnumerateObjects, GetObjectType, ClickObject, GetText, GetProperties,
  SetFocus (AMS script API: Page.EnumerateObjects, Page.GetObjectType, Button.GetText, Page.ClickObject…).

## 2026-10-31 — page objects (verified: Page.EnumerateObjects from inside, logged by the mod)

Page "EN", 13 objects. Buttons (name: text, x,y): Play: Play Game 535,171 · Manual: Game Manual 572,231 ·
Exit: X 862,9 · Eula: EULA 46,473 · Store 179,473 · Register 311,473 · Catalog 46,508 · Forum: Forums 179,508 ·
Website 311,508 · Button1: Patch Notes 751,507. Also LanguageBox (combobox), Web1 (web, the news pane),
Slitherine (hotspot, the logo, no text). Listing all of them through Lua takes under 1 ms.

- The luaL_newstate import hook works; the state appears after the mod first starts, so read it lazily.
- Gotcha: Lua "\30" is decimal, C++ '\30' is octal — use '\x1e'/'\x1f' in C++.
- Keys reach the WH_GETMESSAGE hook on the main thread (= the window's thread).

## Done (2026-10-31): src/core/launcher.cpp

Tab/Shift+Tab cycle language box + buttons in reading order; on a button Up/Down/Left/Right, Home/End, letter
search, Enter/Space = Page.ClickObject. Verified by posted keys: Tab order, arrows, Home/End. Enter not yet tried
(it would launch things). The language box keeps its own keys, letters included.

## Plan (user wants the launcher made accessible, like MBAccess — not skipped)

The buttons are not windows, so the MBAccess method (label the focused control) cannot work. Instead, inside
autorun.exe: capture the lua_State (IAT hook on luaL_newstate), subclass the main window, and on arrow/Tab keys
run AMS Lua on the UI thread: Page.EnumerateObjects → names + types + Button.GetText for the button list,
Page.ClickObject(name) on Enter. Texts and actions come from the launcher itself; no pixel positions.

Fallback, if wanted: skip it with Steam launch option
`"D:\steam\steamapps\common\Field of Glory II\FIELDOFGLORY2.EXE" %command%` or steam_appid.txt = 660160.
