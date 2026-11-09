# Battle

## 2026-10-31 — structures from the PDB (not yet verified live)

- `gBattleMap` (0x8F3E34) CBattleMap: mTiles (_TMapTile*, mWidth x mHeight), mTileData, mWidth +0xC068C,
  mHeight +0xC0690, mSideIDs, mUnitCount[2] (map's starting units), mMapFlags.
- _TMapTile (128 bytes): tile[4] packed terrain (CTileManager::GetTileDefFromPackedTile), currentTerrainObject,
  losFlags +0x58, unit +0x5C (CUnit* on the tile), blockedUnit, maxHeight, UIHeight, pass +0x75.
- CUnit (2980 bytes): mTeam, mUnitFacing, mManCount, mAttrib (TAttribSet: int value[192] + array[8][16]) +0x394,
  mTile (CMapPos x,y) +0x894, mDestTile, mType +0x8A4 (squad template), mSide +0x8A8, mUniqueID, mDead +0xB56,
  mRemoved +0xB5A, mRoute.
- Attribute names: `gSideData` (0x8F3EA8) CSideData: mAttribValue TAttrib[192] +0xA414 (names; CUnit::GetAttrib ->
  CSideData::FindAttribValue), mAttribValueCount; mSquadNames char[28][1280] +0x1410 (internal names by type).
- `gGameView` (0x8F3E88): the battle display? CBattleDisplay: mMySide +0x104DC, mOtherSide, mCurrentUnit +0x104FC,
  mDeployMode +0x104E4. Functions: SelectUnit, GetUnitFromIndex, GetTileOnTile.

## 2026-10-31 — verified live (first tutorial battle)

- Battle running: layers show BATTLECOMMON (+ BATTLEADVANCEDPANEL, UNITINFOPOPUP, TUTORIALPOPNONMODAL).
  `gGame` is a CGame*: mBattle +0x18 (CBattleDisplay*), mGameMode +0x15C (0 in this battle).
- gBattleMap 48x48; reading _TMapTile.unit over all tiles found 26 units. CUnit: mSide +0x8A8, mType +0x8A4,
  mManCount +0x388, mDead +0xB56. mType -> gSideData mSquadNames (internal ids: MEDIUM_WARBAND, CELTIC_CHARIOTS).
- gSideData mAttribValueCount = 137; TAttrib = char tag[32] (attribute names, e.g. AP).
- Display names: language files Data/Text/Text2.txt etc. (UTF-16): IDS_UNITNAME48 "Warband (Loose Order)".
  Engine lookup: gStringStore (0x8F4ACC), CStringStore::GetString / GetIndexedString / FindEntry.
  How a type maps to its IDS_UNITNAME number: not yet found (probably a Squads.csv column).
- Hover: CBattleDisplay::mCurrentTile (CMapPos +0x10534) and mCurrentPick (+0x1053C) are set each frame by
  CBattleDisplay::DoPick (thiscall, no args, prologue 55 8B EC 81 EC B0 00 00 00: 9 bytes, relocatable). DoPick
  checks CUIBlockManager::OverUIBlock, calls a script function, PickUnits, CBattleMap::Pick.
  Plan: hook DoPick (call the original, then put the grid cursor in mCurrentTile) so the game's own hover panels
  describe the cursor tile, and a posted left click lands on it.
- The menu module reads the battle screen as one flat menu and its joining rules fuse unrelated HUD lines
  (unit panel, briefing, scores). Battle needs its own handling; menus only for popups / modal screens there.

## Keys (user, 2026-10-31)

The grid has 8 directions (game scripts handle diagonal steps, e.g. CheckPursuitDiagonalStepNotBlocked), so:
Q W E / A D / Z X C move the cursor (X is south), S reads the unit on the tile. No tile-read key chosen yet: moving
speaks the tile. Enter acts like a left click on the tile (select / order), the game's own rules deciding.

## 2026-10-31 (session 2) — battle module built (src/core/battle.cpp)

Verified live (game minimized, keys sent with tools/sendkeys.py):
- Units: CBattleDisplay::mSides (CSide[2], 762884 bytes each): mUnit CUnit[256] +0, mUnitCount +0xBA400.
  mCurrentUnit = side*1000 + index (GetUnitFromIndex); -1 = none. Our side: mMySide (0 in the tutorial).
- Display name: TSquadTemplate::mID (+0x588) = Squads.csv ID = the number in IDS_UNITNAME<n>. Templates:
  gSideData (a pointer) -> mSquadTemplates[mType]. Type 44 -> mID 48 "Warband (Loose Order)".
- Strings: CStringStore::FindEntry(this, "IDS_...") -> TStringStoreEntry {char tag[64]; uint16 *string +0x40},
  null if missing (GetIndexedString would log a missing string and return a placeholder). gStringStore is a pointer.
- Terrain: _TMapTile.terrain (+0x77) indexes CCombat::mTerrain (TTerrainInfo[29], 140 bytes, char name[32] first,
  OPENGROUND...); names are IDS_TERRAIN_<name> ("Open ground", "Woods").
- Line of sight: _TMapTile.losFlags bit side*8 (0x1 side 0 sees, 0x100 side 1 sees; 0x101 both).
  Enemy units on tiles without our bit are hidden from every reading. CUnit::mSeen (+0xB5C) was 1 for all enemies.
- Faction names: texts BATTLE_SCORESIDE0NAME / 1NAME in BATTLECOMMON ("Ancient British", "Roman").
- DoPick hook installed (prologue checked, 9-byte trampoline; restored on the game thread at unload).
- Enter on our unit's tile: posted mouse move + left button down, up 50 ms later, with the cursor moved to a spot
  no UI box covers -> the game selected the unit (mCurrentUnit changed; spoken "Celtic Chariots, selected").
  Works even while minimized: the click goes through the window messages + our mCurrentTile.
- Scanner over both sides, grouped by display name: "Celtic Chariots 1 of 6, 2 north, 2 east, 2 of 4".
- UNITINFOPOPUP is the Info popup (OK button, title + long text), opened by BAPInfoButton: left to the menu module.

NOT verified (needs the game in front): S reading the game's panel (BATTLE_SELECTEDBOX / BATTLE_TARGETEDBOX in
BATTLEADVANCEDPANEL were hidden while minimized), the hover-text watch after moves, orders (clicking a tile with a
unit selected), north = -y on screen, Ctrl/Alt+PgUp/PgDn (sendkeys can't hold modifiers).

How orders work (from the engine and UI files): click a unit to select it, then click a tile; the game picks the
action from the tile (move, charge, shoot: CBattleDisplay::DoActionOnCurrentUnit, FlagActionableTiles marks the
tiles that take an action). Extra orders are "bonus buttons" (UpdateBonusButtons / TriggerBonusButton,
mSelectedBonus) shown for the selected unit; not yet read by the mod.

## 2026-10-31 (session 2) — clicks without the mouse, orders

- CBattleDisplay::HandleMouse(msg, x, y, wParam, unused) (thiscall, ret 0x14) gets the window's mouse messages
  (msg 0x200..0x20A, jump table). Left button up acts on mCurrentTile, not on x, y: x, y only go to
  HandleMouseClickOnUIBlock (the map's clickable icons) and the drag start. The mod calls it directly with
  x = y = -1: no window, size, resolution or real mouse involved. Verified (also while minimized).
- Control mode = gOptions value 0x17 (0 here). Mode 0: left click only selects our unit / clears the selection
  (SelectUnit(-1, 0)); RIGHT button up runs the order: if mOptionsWalk == 1 and mSingleClick (1 here) ->
  DoActionOnCurrentUnit; with several orders it first marks the tile (mSelectedTile, SetSelectedPoint).
  Mode != 0 does the same on the left button. The user's left-click "move" did nothing for this reason.
- Orders on the hovered tile: CBattleDisplay::mOptions (TOptionEntry[16]: object = the order's icon, cost,
  functionIndex, type, index, priority), count mOptionsWalk; filled a frame after the tile is hovered. The icon's
  tooltip is the order's text: "Move Unit 8/10" (AP cost / AP). Verified live; spoken after each move.
- Enter = right click when a unit is selected and the tile has orders, else left click. Escape = SelectUnit(-1, 0)
  when a unit is selected ("none selected"), else the key goes to the game. Verified: selection, order speech,
  Escape. NOT verified: an actual move / attack by Enter, tiles with several orders.

## 2026-10-31 (session 2) — orders need the engine's mouse on the map; HUD keys

- Why Enter didn't move units with the game in front: CBattleDisplay::GameUpdate fills mOptions only while
  CUIManager::GetControlUnderMouse() (no-arg overload, 0x4B7580 = what SymFromName returns) is the battle display,
  CUIBlockManager::OverUIBlock(gUIBlockManager, gMouse, 0) is false and GetMiniMapPos(pos*) is false. gMouse is a
  pointer to {int x, y} (screen pixels). While minimized gMouse was stale on the map, so it worked then; in front
  the real pointer was over the HUD -> no orders -> Enter was a left click -> selection cleared.
  Fix: each tick (and before a click) keep gMouse on a spot that passes those three engine tests, searched at
  fractions of the battle display's box (first seen: 576,540 = 30%/50%, the middle being under the tutorial popup).
  NOT yet verified by the user.
- Order icon: CUIObject vtable slot 25 (+0x64) returns mEnabled (+0x54); the right-click path only acts when the
  single order's icon is enabled. Tooltips (CKString mToolTipString[3]) come from the order scripts' CHECK_ functions
  (Data/Battle/Scripts/Turn.BSF CHECK_ALL_TURN prints IDS_TT_TOOFEWAPTOTURN, IDS_TT_CANNOTTURN_ZOC... as the reason).
  Mod: all three lines spoken, "unavailable" for a disabled icon; Enter on a disabled one speaks them.
- Language-file markup: <img = left_mouse.dds> / right_mouse.dds stand for the mouse buttons (tutorial: "by
  <img>ing on it"), ~ is a line break. eng::plain now reads them as "left click" / "right click".
- Strength (Data/scripts/UITools.BSF PrintStrengthString): DisplayTotalMen * StrengthMultiplier/100 against
  StartingStrength (base TotalMen * UnitSize / base UnitSize; base = TSquadTemplate::mTemplateAttrib). DisplayTotalMen
  isn't in gSideData's attribute names (script-only?); the mod uses TotalMen (attribute 21, 720 for a warband) and
  ignores the multiplier (100 here). Spoken after the unit's name only when below starting strength.
- HUD (BATTLECOMMON): ENDTURNBUTTONTEXT ("Turn 1\nEnd Turn"; with a turn limit "Turn x/y\nn remaining"),
  BATTLE_SCORESIDE0/1NAME + SCORE, BATTLE_SUBTITLETEXTBATTLENAME / DATE, BATTLE_BRIEFINGINFO, ENDTURNBUTTON then
  CONFIRMENDTURNBUTTON (BattleCommon.bsf). Game hotkeys in battle: B next unshot, C combat log (taken by our C),
  Tab next unmoved, 1 line of fire, 2 line of sight, 3 command range, F1 shortcuts.
- Keys added: T turn + scores, R battle name/date/briefing, N End Turn (then Confirm), Ctrl+Enter = Ctrl+click
  (the game's unit information popup, MOUSE_CLICK_HANDLER in HelperTools.BSF), Escape -> "canceled" (also spoken
  whenever the selection is dropped). The hover-text watch now skips the HUD screens (S, T, R read those).

## 2026-10-31 (session 2) — tiles with several orders

- User log: one unit moved, then tiles offering "Move Unit 12/16. Turn Unit 0/16" (mOptionsWalk 2): a right click
  only marks the tile (mSelectedTile, SetSelectedPoint) and the game keeps computing the orders for the MARKED tile
  (the icons a sighted player then clicks) until the next right click: the same two orders followed the cursor,
  further Enters toggled the mark.
- GameUpdate clears mOptionsWalk/mOptionsValid every frame and refills them (hovered tile, or the marked one).
- CBattleDisplay::DoActionOnCurrentUnit(int option) (thiscall): needs mSelectedTile != -1, runs mOptions[option]'s
  script (GetScriptFromOption). The game sets mSelectedTile = mCurrentTile, calls it, sets -1.
- Mod now: Enter with one order -> DoActionOnCurrentUnit(0) the same way; several -> mSelectedTile = cursor and an
  order list (Up/Down/Home/End, Enter does it, Escape closes = "canceled", any other key closes it); a disabled
  order (icon mEnabled 0) is not done, its tooltip is spoken. No orders -> left click as before. NOT yet verified.
- Alt+Q/W/E/A/D/Z/X/C (user, 2026-10-31): with a unit selected, the cursor jumps to the square next to the unit
  that way and, two DoPick frames later (the game has that square's orders), Enter's order logic runs without the
  left click: one order is given, several open the order list. Any key before then cancels it. The given order's
  text is spoken after it (also for Enter). NOT yet verified.
- Facing: CUnit::mUnitFacing (uint16 +0x2) in degrees; gIVectorFromDirectionTable (GetIVectorFromDirection) gives
  0 -> (0,+64), 90 -> (+64,0), 135 -> (45,-45), 180 -> (0,-64): 0 = +y = the mod's south, 90 = east. Tutorial: our
  army 0 (facing the enemy at higher y), the enemy 180. Spoken after the name ("facing south"), with strength as
  "600/720" when below starting strength (user, 2026-10-31).
- F1 in battle lists the game's hotkeys (read as line items): ESC menu, WASD scroll, QE/arrows rotate, FR/arrows
  zoom, SPACE shut popups / proceed / deselect, "CTRL+L click on unit" (= Ctrl + left click: unit information),
  "CTRL mouse over target - detailed tooltips", 1/2/3 LOF/LOS/command range, L unit list, C combat log,
  K casualties, B units that can shoot, TAB unmoved units, G grid, J minimap, M top view, X map overview, F2 screenshot.
  Mod: Ctrl+direction moves the cursor with Ctrl held, so the orders spoken after it are the detailed tooltips
  (if the order icons' tooltips are what changes with Ctrl; NOT yet verified).

## 2026-10-31 (session 2) — unit list (L), casualties (K), Escape

- L = UNITLIST screen (layer 12, flags 0): UNITLISTLIST (CUIListBox, 17 entries for our 17 units) + UNITLISTCLOSE.
  Entries have no stored text: CUnitList::DrawUnitListItem runs UnitList.bsf UI_ITEM_RENDER(x, y, w, h, me) which
  RenderStrings name, PrintStrengthString, "AP: n", PrintMoraleDisorderString. CUIListBoxItem::data = the unit's
  index (side*1000 + i). Script natives have no addresses (the VM dispatches by index; RenderString's table entry
  at 0x69C838 is a 400-byte record with the name and help text, no pointer). Text goes through
  FTBitmapFont::DrawString(text, pos*, ...) (prologue 55 8B EC 6A FF: hookable) if capture is ever needed.
  Mod: battle_list_entry builds the entry from the unit: name, DisplayTotalMen/start, IDS_TT_AP + AP,
  IDS_MORALESTATE<MoraleState> (disorder part left out). Verified: "Celtic Chariots, 100/100, AP: 16, Steady, 15 of 17".
  Moving in the list does not select the unit on the map.
- When the selection changes by other means (unit list, the game's Tab / B), the cursor now jumps to the unit.
- K: CBattleDisplay::Update on KeyPressed('K') -> CUIManager::EnableScreen("BattleResults") + ShowKills: the
  BATTLERESULTS screen ("Losses": BR_TITLETEXT, BR...TEXT0/1 table texts). A posted K (sendkeys) did not open it
  (KeyPressed may read another key state); NOT yet seen open.
- Escape in menus (user, 2026-10-31): presses the screen's own way out: a button named *CLOSE / *BACK / *CANCEL /
  *OK / *EXIT / *DONE (or containing <that>BUTTON), else one labelled close / go back / back / cancel / ok / exit /
  done; nothing found -> the game gets Escape. Not while the battle owns the keys (Escape stays deselect, then the
  game's system menu). Verified: Escape in the unit list pressed UNITLISTCLOSE.

## 2026-10-31 (session 2) — combat log, undo, deselect, tutorial keys

- Combat log: BATTLECOMBATLOG (BATTLECOMMON) is a TEXTMODE AUTOSCROLL list box, hidden until toggled (C), but the
  scripts add to it regardless: AddVizFunctionCall("PrintUIStringToListBox", "BattleCombatLog", <work string>, colour)
  after each close combat (CloseCombatLogic.BSF ~846, the combat report text) and from MoraleTools.BSF (morale
  results). UseCombatLog() exists but nothing calls it. Combat report popups (ShowCloseCombatReportPopup) depend
  on UseCombatReports(). Mod: new lines spoken (queued) as they appear, also in the enemy's turn. Empty so far
  in the Sicily tutorial: NOT yet verified with a fight.
- Undo: Undo.BSF CHECK_ALL_UNDO, tooltip starts with IDS_TT_UNDO; offered on the square the unit came from.
  Enter leaves it out (with only Undo there Enter does nothing); Alt+Enter gives it (user, 2026-10-31).
- After an order is given the unit is deselected (SelectUnit(-1, 0), no "canceled").
- Language-file mouse pictures now read as the mod's key: <img left_mouse/right_mouse> -> "Enter", followed by
  "ing" -> "pressing Enter" (user, 2026-10-31). NOT yet verified.

## 2026-11-01 — events and buffers (built, NOT yet run: the game was closed)

- Turn changes: EnemyTurnImage (BATTLECOMMON) is shown only in the other side's turn (BattleCommon.bsf
  BattleCommon_SetUI: GetShowSide() != gDisplaySide); its text EnemyTurnText = IDS_ENEMY_TURN. Mod speaks it when
  shown, and the first line of ENDTURNBUTTONTEXT ("Turn 2") when hidden again.
- Enemy units: per unit (both sides tracked) position, stood-still ticks, seen (visible() = our LOS), morale.
  An enemy unit that moved and stood still 30 ticks (~0.5 s) -> "<name>, <faction>, moved to x, y, <offset from
  cursor>"; one coming into view -> "spotted". The first look at a battle only records. CUnit::IsMoving exists
  (0x548460) but wasn't used.
- Morale: MoraleState attribute (index 46 here) changing on a visible unit -> "<name>[, faction], IDS_MORALESTATE<n>".
- Deployment: DEPLOYMESSAGE screen, DEPLOYMESSAGETEXT (DeployMessage.BSF) spoken when it changes. N also presses
  ENDTURNBUTTON2 (BattleCommon.bsf's other End Turn button; it triggers the engine's DeployFightButton). Placing
  units is expected to work through Enter's left click (HandleMouse: deploy mode 1 -> StartUnitMove on our unit,
  EndUnitMove on button up / next click). Untested; deploy zones aren't spoken yet.
- Buffers (user, 2026-11-01, MBAccess-style): everything the battle announces on its own (combat log lines, turns,
  moves/spotted, morale, deployment messages) is kept by category (All, Combat, Morale, Movement, Turns,
  Deployment; 1000 entries, cleared per battle). Shift+Backslash (VK_OEM_5) opens them as a menu: Left/Right
  category, Up/Down entries (opens on the newest), Home/End, Escape or Shift+Backslash closes ("closed"); any other
  key closes it and acts. H / Shift+H removed.
- Buffer keys changed the same day (user): no menu and no Shift+Backslash. [ ] switch category (speaks the
  category and its newest entry), comma / period older / newer entry; Shift+[ / Shift+] first / last category,
  Shift+comma / Shift+period oldest / newest. Same keys as MBAccess's buffers/messages.
- Game keys 1/2/3 (keys.bsf KEYBOARD_LOS) only toggle overlays (gShowCommandRange...). Spoken instead (user,
  2026-11-01): "out of sight" on cursor squares without our LOS bit; "out of command range" for our units whose
  GeneralInCommandRange attribute (125 here; set by GeneralInCommandRange(id, 1) in the scripts) is -1. Line of
  fire: the shoot order is offered only on enemies the selected unit can shoot. Built, not yet run.

## 2026-11-01 — special orders, force selection (built, NOT yet run)

- "Bonus buttons" (CBattleDisplay::mBonusButtons / TriggerBonusButton, global orders): GlobalOrders.BSF is a dummy
  ("until global orders are added to main build"), so there are none. The unit's other orders are ALL_ order
  scripts (CHECK_ALL_*: FallBack, FormSquare, MountDismount, Move_Command, Turn_Command, Switch_Command,
  Move_General, NameUnit, PassThrough, AddGeneral, FightMelee, Turn, Undo, Move): ordinary tile options, already
  spoken and given (own-square ones through Enter's order list).
- Force selection: ForceOverlay screen (Data/UI/ForceOverlay.txt): ForceOvly_List (listbox: entries " <unit name>",
  data = unit type, drawn by ForceSelect_RenderListItem), ForceOvly_PointsBox ("remaining/total"), buttons
  Autofill, Autodeploy, Accept, sort by type/points/quality, Back, Settings, LoadSave, ShowBriefing, Help.
  Buying: ForceSelect_ListBox.BSF UI_OBJ_UPDATE(mousex, mousey, buttons, over, dragging, flags, data): left button
  over entry `data` buys one (cost <= gForcePointsRemaining, selected < unfixed; marks a unit ForceSelectSelected,
  RoughDeploy), right button sells; gForceSelectTicks debounce (15).
  On the map, MOUSE_CLICK_HANDLER action 1 (= right click; left click is action 0) un-buys a bought unit.
- Script calls: CUIObject::mScript (+0x44) CCScriptInstance*; CCScriptInstance::GetFunction(name, int *extra)
  (0x46AD40, what SymFromName returns) -> index or -1; CCScriptInstance::CallFunction(index, int args[12], extra)
  (0x46C880; SymFromName returns the by-name overload 0x46CA70, whose first E8 call is the index one). Args go by
  parameter position (DrawUnitListItem matches names to fill them). eng::call_script.
- Mod: in FORCEOVLY_LIST, Right = buy one of the selected entry's type, Left = sell one (UI_OBJ_UPDATE with
  buttons 1 / 2, data = selected index), then the changed texts (points box) are spoken.
- Unneeded game keys dropped while the battle owns the keys and nothing else took them (user, 2026-11-01):
  J, G, M, F, R (the mod's R still works: it is taken first), 1, 2, 3, arrows (camera). Tab, B, L, K, Space,
  Escape, F1, F2 still reach the game. battle_blocks(), applied in menus.cpp's hook after battle_key and key.

## 2026-11-07 — user requests (built, NOT yet run)

- User hadn't found End Turn: it is N (then N again for Confirm). Log showed N never pressed.
- Scanner: units in a type sorted nearest first from the cursor (Chebyshev distance, 8-direction moves), types
  ordered by their nearest unit. Recomputed on each key, so after Home the next item is the nearest to there.
- Alt+direction orders keep the unit selected (from_step); only Enter's order (unit selected, Enter on the
  destination) deselects. If the game deselects by itself after a move, that's still to see.
- S: "AP: n, <name>, men/start, <morale>, facing ..., <game panel>" (unit_stats shared with the unit list).
- Generals (UnitList.bsf): attribute General > -1 = a general with the unit; GeneralType 0/1/2 ->
  IDS_GENERAL_TYPE<n> "Sub-General" / "C-in-C" / "Ally General". Spoken after the unit name on the cursor, S, the
  unit list. Wording (user, 2026-11-07): "<name>, has general, <rank>". Scanner: a general's unit stays in its
  kind's group; third category "Generals" (Ctrl+PgUp/PgDn cycles ours / enemy / generals): each general its own
  item, both sides (enemy ones end with the faction), nearest first, no "n of m" inside.
- Tab in battle went to the menu module (the tutorial box's LINE items + Proceed), read the tutorial and never
  reached the game. Now menus key() leaves Tab to the game while battle_owns() (next unmoved unit; the cursor
  follows the new selection). Tutorial box: Up/Down, Space.
- Order text (user, 2026-11-07): tooltip lines split on line breaks; a line with "the <X> icon" (the click
  instruction, after its descriptive sentence) gives the name X and is dropped; trailing periods dropped:
  "Shoot, Half Arc of Fire, Casualties: 4 - 16". English-only name pick; other languages fall back to the full text.
  Fixed same day: the tutorial text breaks the line inside ("on the~Turn and Shoot icon"), so the match runs on the
  whole string and erases the lines it spans; outside tutorial mode the first line is "Turn and Shoot:" -> name.
- Turn and Shoot (TurnAndShoot.BSF CHECK_UNIT_TurnAndShoot) is offered at all only if the unit has range, the target
  isn't already in full arc, the turn is allowed (CHECK_ALL_TURN: AP, enemy zone of control) and the shot after it
  is valid; otherwise the game shows no icon and the plain Shoot says not in arc. The AI never uses it.
- Selection by other means (Tab, B, unit list): "<name>, selected, <offset from the old cursor>".
- Charge tooltip outside tutorial mode is only "Impact: Win x%, Draw, Lose, Melee: ..." -> name IDS_TT_CHARGE
  ("Charge" in Text1; Text2 reuses the id for a long sentence, so a > 20 char result falls back to "Charge").
- Combat log NOT spoken after a real charge (log 21:47:14). CloseCombatLogic.BSF:846 adds the line unconditionally
  via AddVizFunctionCall (after the animation). Diagnostics added: "combat log: found / not found / n lines".
  Alternative the game offers: Settings "Close Combat Reports" Simple/Detailed -> ShowCloseCombatReportPopup.
- LOS BUG fixed (2026-11-07): _TMapTile::GetLOS(side) (0x401B70, disassembled live) = 1 if the tile's unit has
  CUnit::mForceVisible (+0xB57), else (losFlags >> side*8) & 0xFF (a whole byte; non-zero = seen). The mod tested
  only bit 0 -> seen squares said "out of sight" and enemy units on them were missing from the scanner/cursor
  (user: "only one legion left" on turn 3). battle.cpp seen().
- Score "0% (0%)" is right: UITools.BSF PrintRoutedString = 100 - UniversalVar SideNRemaining (army break
  progress from routed/lost units, not men lost); the bracket is the difference to the other side.
- T now "Turn 4. Ancient British 0%, Roman 7%" (user): drops ENDTURNBUTTONTEXT's last line (always
  ids_EndTurnButton, BattleCommon.bsf:633/637) and the score's "(+7%)" difference (colour-coded on screen).
- Detailed (Ctrl) charge tooltip starts "Impact, Points of Advantage" -> also gets the Charge name.
- General rank only on S and in the Generals category; elsewhere just "has general".
- Unit list: Enter on an entry = CBattleDisplay::SelectUnit(data, 0) (battle_list_pick), then the list's Close.
- F1 in battle: the mod's key list (key_help in battle.cpp; keep it in step with battle_key), Up/Down/Home/End,
  Escape or F1 closes. The game's own F1 list is no longer reachable while the battle owns the keys.
