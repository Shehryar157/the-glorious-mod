# Status (last updated 2026-10-31, session 2)

## User's rules (shared by all their access mods)

- Efficient solutions over clunky simple ones; keep keypress-to-speech latency minimal.
- Explain the technical background (what the game is made in, how the mod runs) as we go.
- Only the game's own words and plain state words; no mod-written hints or help.
- Menus: positions ("3 of 9"), Home/End, real type-search (typed prefix, same letter cycles); sections become tabs;
  make existing buttons accessible where they are. Non-clickable texts are not items: they label controls, or are
  headings (only when a screen has several; a lone one is the title, spoken on open; a text of several lines on a screen without sections becomes read-only line items, user 2026-10-31). Toggles/choices are one
  control (Enter cycles), sliders change with Left/Right; buttons that open something stay buttons.
- Grids stay grids. Battle map (square grid, 8 directions): Q W E / A D / Z X C move, S reads the unit,
  Enter = left click on the tile (user, 2026-10-31). Tile-read key not chosen yet (moving speaks the tile).
  Scanner: Ctrl+PgUp/PgDn category, PgUp/PgDn item, Alt+PgUp/PgDn same-kind items, Home go to, End distance.
- Usage is limited: batch steps, keep tool output short (grep/tail), write finished code in larger pieces.

## What works

- Loader: version.dll proxy loads into both autorun.exe (launcher) and FIELDOFGLORY2.EXE; hot-reloads
  FoG2Access.dll on every `python tools/build.py` (no restart needed).
- Launcher (launcher.md): Tab/arrows over language box + 10 buttons via AMS Lua; Enter on Play Game starts the game.
- Game menus (menus.md): screens from the top layer down to the first covering one (flag 1); layout rebuilt into
  controls: buttons (text or tooltip), choices/toggles (indented button = selected), sliders (progress bar),
  value texts, headings/title, selected tabs. Settings screen verified. Volume sliders verified working.
- Lists/drop-downs (menus.md): built and loaded, NOT yet tested by the user.
- Battle (battle.md, src/core/battle.cpp): grid cursor (unit, terrain, coordinates), S, Enter = left click
  through CBattleDisplay::HandleMouse (no real mouse: window/resolution independent), orders offered on the tile
  spoken ("Move Unit 8/10"), Escape drops the selection, scanner (both sides, grouped by unit name), DoPick hook. Menus skip the battle HUD
  screens while the battle owns the keys; arrows/Tab/Space still work on popups there.

## Open problems

- Toggles: pressing On/Off had no effect in tests, but the game window was minimized then (scripts seem paused
  while minimized). Needs a test with the game in front; if still broken, investigate why Trigger(0,0) doesn't
  reach the screen script (CUIObject::Message -> ProcessMessage -> script UI_OBJ_HANDLE(data, event, id)).
- Battle, untested with the game in front: S's panel text, hover texts after moves, an order actually carried
  out by Enter (right click), tiles with several orders, north = -y, Ctrl/Alt+PgUp/PgDn. Enter on a tutorial popup goes to the battle (Space presses it).
- The improved loader (per-process live copy + deleting stale copies) is built but not installed: run
  `python tools/build.py install` with the game AND launcher closed. Until then: if a stuck game process holds
  FoG2Access.game.live.dll, rename that file (allowed while loaded) and run build.py to reload.

## Source layout

- src/loader/proxy.cpp + version.def: proxy exports, Lua state capture (launcher), main thread id, hot reload.
- src/core/mod.cpp: mod_start/mod_stop, picks launcher or game by exe name.
- src/core/engine.cpp/.h: symbol lookup through the game's dbghelp (78 ms), RTTI kinds, CKString, Trigger,
  list boxes (count, entry text, click-style select).
- src/core/menus.cpp: game menus (WH_GETMESSAGE hook + 16 ms timer on the game thread); drives battle.cpp too.
- src/core/battle.cpp: battle map grid, unit/terrain speech, click, scanner, DoPick hook.
- src/core/launcher.cpp: launcher keys and AMS Lua calls. src/core/speech.cpp: NVDA + shared log (FoG2Access.log).
- tools/syms.py (PDB: find, addr, at, layout, types), tools/mem.py (read game memory; picks the biggest game
  process), tools/walk.py (live UI tree of the screens that are up; `all` = every loaded screen),
  tools/disasm.py (disassemble live code: the exe on disk is encrypted by Steam DRM),
  tools/sendkeys.py (post keys to the game, else the launcher; prints new log lines),
  tools/build.py (build / install / uninstall).
- native/nvdaControllerClient32.dll: NVDA 2025.3.2 controller client, x86.

## 2026-11-07

- Toggles verified working (Ambient Sounds Off -> On with the game in front).
- Menu slowness = the game loading from the HDD (menus.md 2026-11-07); mod now says "Loading". Built, untested:
  edit boxes (save name), scanner by distance, Alt+direction keeps selection, S with AP first (battle.md).

## 2026-11-09 — what's really left to build (survey)

- Special orders: nothing to build (GlobalOrders.BSF is a dummy; the other orders are tile orders, already spoken).
- Campaign text screens: handled by the general menu reader (menus.md); never seen live.
- Deploy zones: the game's check is per unit (Move.BSF DEPLOYCHECK_ALL_MOVE -> CheckDeploymentStandard: rectangle from
  UniversalArray DeploymentX/Y, separate for light troops, minus beyond-river; or scenario CheckDeploymentBespoke).
  With a unit selected the tile already offers "Deploy" / "Swap units" (IDS_TT_DEPLOY / IDS_TT_SWAP_UNITS) through
  the existing order speech. Still to find live: where the highlighted zone is kept (CBattleDisplay mDeployTile
  textures +0x1037C; disassemble FlagActionableTiles 0x5D01F0 / the tile draw) to say "deploy zone" without a selection.
- Force selection entry details: drawn by script (ForceSelectionTools.bsf ForceSelect_RenderListItem): cost =
  GetUnitTypeCost(type), units column = fixed+sequestered [+ selected-sequestered / unfixed-sequestered] from the
  script global gForceSelect[item]. Needs reading script globals live (CCScriptInstance +0xC TScriptInstanceGlobals;
  CScriptHelper::FindVar 0x440B50; __TScriptVariable data/type/arrays).
- Installer draft: src/installer/installer.cpp (postponed by the user).

## 2026-11-09 — built and verified live (game minimized, keys via sendkeys)

- Improved loader installed (build.py install).
- Force selection rows: "Iranian Armoured Cavalry, Cost 60, Quantity 1 + 0/6" (labels = column headers). Script
  globals read by name: eng::script_var / eng::script_member (CScriptHelper mVarNames buckets -> mVars; mStruct).
  gForceSelect[row] {typeIndex, fixed, unfixed, selected, sequestered, sort, allied}; typeIndex indexes
  CSideData::mSquadTemplates, cost = TSquadTemplate::mCost (fixed units' costs = points spent, checked).
  Right / Left speak the new quantity, then the points box. Verified.
- Button presses that add/remove controls on the same screen (Autofill) no longer drop the changed-text watch.
- Deployment: with a unit selected, squares in its area speak "Deploy here" (the game's own per-unit check);
  Enter deploys (single order done at once). Verified. Squares outside say nothing. Blank order icons dropped.
  The editor's painted zones are TMapTileData.data[0] (side bits 6-7, area 1-based), drawn only in the editor.
  Deploy message: spoken once (menu module), kept in the Deployment buffer.
- End Turn prompt (user): first N -> "Turn 1/24, 23 left. Confirm, 1 of 1"; Enter or N confirms, Escape takes it
  back ("canceled", presses ENDTURNBUTTON2), other keys take it back and act. Decided by the screen script's
  gEndTurnConfirm (its update, which draws Confirm, doesn't run while the window is minimized). Verified;
  Up/Down repeat untested.
- S: name first, then AP (user).
- Selection chatter (user: "so much stuff about units moving" after End Turn): the game moves the selection by
  itself while playing a turn; now only selections within 1.5 s of a user key (and held 10 ticks) are spoken.
  Built, not yet seen in a clean turn. Enemy moves / losses / combat results are still spoken by design.
- R = retreat (BATTLERETREATNEW, tooltip IDS_TT_EXIT_BATTLE): first R asks "<tooltip>, 1 of 1", Enter / R
  retreats, Escape cancels. Briefing moved R -> O (user). Built, not run.
- End-of-battle screen (ENDBATTLE) seen live by the user: "My Lord, we cannot conceal that this was a" /
  "DEFEAT" were two items -> wrapped lines now join (prev ends lowercase/comma, next lowercase or all caps).
  Difficulty choice spoke "1 of 6, Centurion": now "Difficulty, Centurion" (nameless choice takes the text
  right above it). Built, not verified.
- Hills (user): the game's terrain tooltip (Helpers.bsf HELPER_TERRAIN_TOOLTIP, called from
  CBattleDisplay::Render via CallScriptFunction(name, bd+0x148 script, params, 0); output in
  *(bd+0x10614)+4 CKString) says "<terrain> (Hill). ... Height: 100/200/300" (IDS_TT_HILLS,
  IDS_TT_TILEHEIGHT): heights only differ by the number. GetTileHeight (CBattleScriptHandler::
  CallCustomFunction ~0x5A9F4C) = CBattleMap::GetFixedPointHeight(x*256+128, y*256+128) * 100 >> 8. Corner
  heights: CBattleMap::mFixedHeightOriginal (+0xC0688, int16[(w+1)*(h+1)]); first tutorial hills = 512 -> 200.
  Mod: cursor says "Open ground (Hill), Height: 200" (tile_height calls the engine function). Built, not heard.
- Final Losses (BATTLERESULTS) was read in screen order, both sides interleaved (user). menus.cpp
  results_table(): per side (names: …1/ENEMY/BR_ = side 1, …0/FRIENDLY/BR2_ = side 0; left panel first), a LINE
  per row, values paired with the nearest column header. Same day (user): made a TABLE item instead: a LINE
  per side ("Roman (Original Strength 3120)"), then one table, rows Killed / Wounded / Captured/Deserted /
  Engines Lost / Elephants Lost / Total Lost, columns "<side> Infantry/Cavalry/Total" for both sides (a lone
  value goes under Total). Up/Down rows (past the ends: next/previous item), Left/Right columns, Home/End
  first/last column; a changed row/column says its header, then the cell ("blank" if empty, "edge" at an
  edge). Built, not run.
- Battlefield review (EndReview.txt: wreath, ENDREVIEWTEXT ids_End_Review, ENDREVIEWOK) made the battle module
  step back (screen with a button = popup). Now ENDREVIEW is skipped by compute_owns and the menu module (HUD),
  its text is spoken when it appears, N presses ENDREVIEWOK. Built, not run.
- Campaign chooser (user: "moving to the list moves you into the choosing menu"): the list was one LIST item
  (arrows changed the campaign), and a changed description (CAMPAIGNINFOTEXT, set by QuestControls.bsf
  UI_OBJ_RENDER from the selection) counted as a new tutorial page -> jump to its first line. Now
  CAMPAIGNNEWLIST and SCENARIOS_LIST entries are ENTRY items (one per entry, selected on arrival, description
  read by the text watch, Enter = Launch/Accept), the screen opens on the selected entry, and a description
  change while on a list/entry doesn't move the user. Verified: Up/Down move through campaigns and select them.
  Description read after the name: verified (later the same day). Markers (QuestControlsTools.BSF
  PopulateCampaignList; GetCampaignEntry flags bit0 locked, bit1 core, bit2 carryover, bits16-23 DLC):
  " $ (<pack>)" -> ", This content requires these expansions be installed: <pack>" (IDS_LOCKED_EXP_PACK);
  " *" (not core) -> ", user content" (no game words for it). Verified: "Sargon the Great, This content
  requires these expansions be installed: Swifter than Eagles, 29 of 37", then its description.
- Seen once, unchecked: "Eastern Archers, Pontic, moved to ..." while the player seemed to be Pontic: check that
  the faction label is only added to enemy units (mMySide) in a custom battle.

## Next steps

0. User plans to download (2026-11-07) the FoG2 tabletop mod (many new units) and community scenarios: test that
   their unit names, screens and scripts read correctly (names/attributes are looked up by name, so most should).

1. Run the 2026-11-01 build in a battle: turn / enemy-move / morale / combat log speech, the buffers
   ([ ] , . keys), deployment messages; then the earlier untested list (order list, Alt+direction, Alt+Enter undo,
   Ctrl+direction detailed tooltips, K casualties, popups as lines, Escape in menus).
2. Deployment proper (second tutorial): deploy zones on the cursor, placing units. Force selection (third): test Right/Left buy/sell, entry details (cost, counts) not read yet.
3. End of battle (results screen), bonus buttons (special orders), campaign screens, menu lists, loader install.
