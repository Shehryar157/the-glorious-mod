# Basics

Game: Field of Glory II (Steam app 660160, build 14174568), installed at
`D:\steam\steamapps\common\Field of Glory II`. Developer Byzantine Games, publisher Slitherine.
User data: `Documents\My Games\FieldOfGlory2`.

## 2026-10-31 — engine and executable (verified by reading files)

- `FIELDOFGLORY2.EXE`: 32-bit (i386) C++ program, Direct3D 9, Miles Sound System (`Mss32.dll`), MSVC 2008 runtime (`msvcr90.dll`).
  Built on Slitherine's in-house **Archon** engine (source path `..\..\Archon\...` in the exe; the
  same engine family as Battle Academy 2, Pike & Shot, Sengoku Jidai). Project name in the PDB path: `BA2_TILE_SHADER`.
- **No ASLR** (DllCharacteristics lacks DYNAMIC_BASE), image base 0x400000: addresses never move between runs, only between game patches.
- **Matching debug symbols ship with the game**: `STUB_TILE_SHADER.pdb` (GUID in the exe's RSDS record matches).
  `Legion2.pdb` does NOT match (left over from another build). The PDB has the C++ class and function names:
  CUIManager (122 functions), CUIObject (94), CUIListBox, CUIButton, CUIText, CUIEditBox, CBattleDisplay (362),
  CBattleMap, CUnit, CGame, CCScript/CCScriptManager (the script VM), CStringStore (localized strings), COptions.
  UI functions of note: OnFocusIn/OnFocusOut, RequestFocus, GetControlUnderMouse, ActivateScreen, IsScreenTop,
  GetString, ListGetSelectedItem, HandleKeyboard, Confirm.
- Imports system DLLs that are not KnownDLLs and so load from the game folder first: `version.dll`, `winmm.dll`,
  `d3d9.dll`, `dbghelp.dll` (dbghelp ships in the folder). Any of these can be a proxy loader.
- Steam: imports SteamAPI_Init but not SteamAPI_RestartAppIfNecessary, so the exe can be started directly
  (needs Steam running; outside a Steam launch it needs `steam_appid.txt` = 660160 — not yet tested).

## 2026-10-31 — data formats (verified by reading files)

- UI layouts: plain text `.txt` files in `Core/UI` and `Data/UI`. `[ObjectName]` blocks with `TYPE` (display, image,
  text, button, listbox…), position, `PARENT`, `string IDS_...` (string id), `SCRIPT x.bsf`, `HANDLER`.
- Logic: **BattleScript** `.BSF` files (C-like; compiled by the engine at load) in `Core`, `Data/scripts`,
  `Data/UI`. UI scripts implement UI_OBJ_INIT / UI_OBJ_HANDLE(id) / UI_OBJ_RENDER and call engine functions
  like SetUIObjectVisible, TriggerUIObject, GetUIObjectUserData. BattleScript cannot call outside DLLs.
- Localized text: `Data/Text` (string ids IDS_...), handled by CStringStore. `Core/LANG.TXT` lists languages;
  the game has its own in-game language selector (MAINLANGSELECT).
- `lua5.1.dll` in the folder is NOT used by the game exe (no lua imports); it belongs to the launcher.

## Planned loading method

32-bit proxy DLL (e.g. `version.dll`) in the game folder → loads the mod core DLL. The core hooks engine functions
at fixed addresses taken from the PDB (UI focus, screen activation, text drawing) and reads engine objects using the
PDB's type layouts; speech through 32-bit `nvdaControllerClient32.dll`.
