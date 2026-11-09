# Menus

## 2026-10-31 — UI structure (verified live with tools/walk.py, tools/syms.py, tools/disasm.py)

- `gUIManager` (0x8F44C8) is a `CUIManager*`. Screens that are up: `mLayers[24]` (TUILayer {TUIScreen*, CMFrame*}).
  `mScreens` is every *loaded* screen (tutorial popups etc. included), not what's shown.
- TUIScreen: filename (screen name), screen (root CUIObject*), hidden, next.
- CUIObject: mVisible, mEnabled, mBoundingBox (real pixels, 1920x1080 here), mName[32], mToolTipString[3]
  (CKString), mChildren / mSibling tree, mType (1 button, 4 display, 5 text, 7 listbox, 8 image).
- CKString = {uint16 *mText, int mSize, int mLen}: UTF-16, already in the current language.
- CUIButton: mString (its text), mIsCheckbox/mIsChecked, mGreyed. Icon buttons have no text but a tooltip
  ("Language", "Game settings and preferences", ...). CUIText: mString.
- Off-screen leftovers stay "visible" with nonsense boxes (MAINCAMPAIGN at -430,-351): filter on the box.
  Invisible blocker buttons (MAINMENU_BLOCKER_*) have no text or tooltip.
- Main menu: layer 0 MAINMENU (pictures), layer 1 MAINMENUOVERLAY (buttons). The Battles submenu buttons
  (MAINFIGHTNOW, MAINQUICKBATTLE, ...) are in the same screen, shown/hidden.
- Pressing: virtual `CUIButton::Trigger(int data, int flags)`, vtable slot 34 (0x88). Hotkeys call it with (0, 0);
  the mouse with (x, 0). It does group selection, tutorial check, commands, click sound, then the screen script.
- The exe's code is encrypted on disk (Steam DRM wrapper); read code from the running game (tools/disasm.py).
- The game reads the keyboard through window messages (no DirectInput).

## 2026-10-31 — Settings and controls (verified live)

- Toggles are separate buttons per choice ("On"/"Off", "Off/Simple/Detailed", six picture buttons for difficulty);
  the active one has mIndented = 1. OPTIONS.BSF sets this every frame from the options (Options_SetUI in
  UI_OBJ_RENDER) with SetUIButtonSelected, and changes options in UI_OBJ_HANDLE(data, event, id).
- Volume: CUIProgressBar mProgress in thousandths of a percent (100000 = 100%), with -/+ buttons (tooltips
  "Decrease/Increase music volume") handled by the engine, not the script.
- Trigger(0, 0) = a left click (HandleMouse passes 0 left, 1 right, 2 middle; Update passes (1, 0) for gamepad).
- Pressing On/Off with the game window minimized changed nothing (COptions::mOptions at gOptions+0x8C8 unchanged,
  even with a posted real click), while volume buttons worked: script handlers seem not to run while minimized.
  Not yet confirmed with the window in front.
- Script TriggerUIObject sets gUITriggerID/gUITriggerEvent; CUIManager::Update then calls Trigger(event, 1).
- Settings screen extras: OPTIONSLEFTPANELBUTTON1 "More Options" icon under the Music label, OPTIONSCREDITSBUTTON,
  OPTIONSEXITGAME (top right). Two blocks of controls side by side; Accept centred under both.

## Implementation (src/core/menus.cpp, engine.cpp)

engine.cpp looks up every address/offset by name through the game's own dbghelp.dll at mod start (78 ms).
WH_GETMESSAGE hook + 16 ms thread timer on the game's main thread; the list is rebuilt each tick and spoken when it
changes. Screens are taken from the top layer down to the first with flag 1 (covers what's below).
Layout rebuilt into controls: blocks (controls within 40 px sideways / 60 px vertically), rows (centres within
20 px), captions (text centred on a button), sliders (row with a progress bar), choices (row with an indented
button), value text (text row <= 8 px under a control row), headings (other text rows; only used when a screen has
2+, else the topmost is the title, spoken on screen open). Texts join the block of the nearest control by
dx + 4*dy + |centre difference|.
Lists (2026-10-31): CUIListBox is derived from CUIText (check LISTBOX before TEXT). Entries: mItems CKArray of
CUIListBoxItem (56 bytes, CKString string +4; table columns separated by tabs), count via CUIListBox::GetItemCount,
mSelectedItem, mIsDropdown. A click: SetSelectedItemIndex(index, false) then CUIObject::Message(mID, 0, index)
(what CUIListBox::HandleMouse sends). CUIListBox::HandleKeyboard only does Ctrl+C. Texts in a strip <= 60 px
above a list are its column headers. Texts that change within 1.5 s of a pick are read after it.
Game text carries markup (<colour=..>, <f2>, <a>, <img = ...>): stripped. Tabs (Single Player / Multiplayer) are
buttons shown pressed in (mIndented): spoken "selected".
Not yet handled: battle (keys would clash with map controls).

## 2026-10-31 (session 2) — popups in battle

- Unit information popup (UNITINFOPOPUP, Ctrl+click on a unit; mod: Ctrl+Enter, verified opening): title (unit
  name) + one long text + Close. With fewer than 2 headings the menu module cleared them and spoke only the topmost
  as the title, so the description was lost. Now: no sections -> the screen opens with all its texts top down.
- Repeats ("Proceed, 1 of 1", "Close, 1 of 1" over and over): refresh re-announced the current control whenever
  the control list changed (other screens coming and going). Now only when its own speech changes; same controls
  with a new text (the next tutorial page) speak that text once.
- Battle keys vs popups: a screen above BATTLECOMMON that isn't HUD, isn't *NONMODAL and has a visible button now
  counts as covering (battle.cpp has_button), so Enter/Escape/letters go to the popup. The tutorial's
  TUTORIALPOPNONMODAL stays non-covering (Space presses Proceed). NOT yet verified.
- UNITINFOPOPUP has flags 0, so the menu module also walked down into the battle HUD under it (21 items, the flat
  HUD menu again). battle_popup(screen) (battle.cpp) is now a stop for the menu walk too. Verified: after the
  reload the popup read "<unit name>. <full description>, Close, 1 of 1".
- The unit popup's description is set a frame after the screen appears: the first announcement had only the title,
  joined with the tutorial box's text (TUTORIALPOPNONMODAL, layer 13, above UNITINFOPOPUP, layer 11). Now a new
  screen is announced 60 ms after its controls last changed (settle_at), and while a battle popup is up the
  *NONMODAL screens are left out of the menu walk. NOT yet verified.
- Long texts as lines (user, 2026-10-31): on a screen without sections, a text of several lines becomes LINE items
  (one per non-empty line, ahead of the controls; read only: Enter/Space/Left/Right do nothing), so the unit popup
  and tutorial boxes are read with the arrows. One-line texts stay the title. LINE identity is its position, so a
  new tutorial page is the same controls with other line texts: then the first line is announced. NOT yet verified.
  (This relaxes the "non-clickable texts are not items" rule for long texts only.)
- A line ending in ':' ("Shooting:", "Impact:") is joined with the line after it (user, 2026-10-31).
- Slowness (2026-10-31 23:27-23:31): pressing Tutorial (main menu) and then entering the scenario screen each froze
  the game ~70 s (earlier sessions: 12-15 s); keys pressed meanwhile all ran at once afterwards. The PC had 1.2 GB of
  12 GB RAM free (paging). Tick timing added (menus.cpp: "slow tick" > 4 ms, "no tick" > 1 s): after a reload only
  the first two ticks were slow (184, 56 ms: caches warming), then none. So the freezes are the game loading, not
  the mod; recheck with "no tick" lines next time it happens.
- Scenario lists: Enter on a list entry only selects it (the game starts with the screen's Accept button).

## 2026-11-07 — slowness confirmed as the game's loading; edit boxes

- Log 12:07: "pressing MAINTUTORIAL" then "no tick for 15516 ms", then 12 s more entering the scenario. The game's
  thread itself is stuck (no messages pumped), so the mod can't speak either. The game is on D:, a 5400 rpm laptop
  HDD (TOSHIBA MQ01ABF050); C: is an SSD with ~31 GB free, the game is 4.1 GB. Real fix: move the game to the SSD
  (Steam: Properties > Installed Files > Move install folder).
- Mod: a watcher thread (menus.cpp watch_loading) says "Loading" once when ticks stop > 1 s right after a press.
- Load/Save screen (Core/UI/LoadSave.txt): LSCancelLoadSave + LSExit (both "Go Back"), LSList, LSLoad, LSDelete,
  LSSaveNew ("Save"), LSText = the name EDIT BOX. CUIEditBox derives from CUIObject only (own CKString mString
  +0x10C), so gather() skipped it: saving was impossible.
- Edit boxes are now EDIT items: labelled by a text left of / just above them, else their tooltip; "<label>, edit,
  <text>". Enter = CUIManager::RequestFocus(obj) (thiscall, 0x4B94F0), "editing". While editing, keys go to the game
  (Enter too); Tab/Up/Down call ClearFocus() and move; Escape only leaves the box. Typed/deleted characters are
  spoken from the text diff each tick. Battle keys are skipped while an edit box has focus. NOT yet verified.
- "Loading" verified (19:59 log). Keys pressed during a freeze still run all at once afterwards.
- Doubles: the campaign details screen lies over the chooser without covering it (both have Go Back / Settings /
  Load/Save: "Go Back, 5 of 10"); Load/Save has LSCancelLoadSave + LSExit. A button with the label of one already
  taken is dropped if on a lower layer or within 60 px (El.layer; top screen gathered first).
- A new screen opens on its first non-drop-down list (title still spoken) unless it starts with LINE items.
- Enter/Space on a list entry presses the screen's start button (name ends ACCEPT/LAUNCH/START/PLAY, else label
  accept/launch/start/play): tutorial SCENARIOS_ACCEPT, campaigns "Launch". The unit list selects instead.
  NOT yet verified (all three).
- Campaign text screens (Data/UI, Core/UI): CAMPAIGN_OVERLAY (Campaign_Overlaytitle + Campaign_Overlaytext, no
  button), EndCamp (EndCampaignText + EndCampaignOK), EndBattle, Results, BattleResults, TimedPop (goes away by
  itself), StartTurn, ScenarioNamePop, PlayOn, EndReview; campaign chooser in QuestControls.txt (CampaignNewList,
  Scenarios_List, CampaignInfoText). Gap fixed: a screen with only one-line texts made no items, and refresh
  returned before speaking: now its title text is spoken when it appears/changes. Multi-line texts were already
  LINE items. None of these campaign screens seen live yet.
- 2026-11-08 combat (battle.cpp): the combat log DID come through (log 22:21-22:48, up to 250 rows), but as
  display rows cut mid-sentence, and shooting rows have no casualty count. Now: new rows are one buffer entry
  (not spoken) and only "Result: ..." is spoken; watch_units speaks "<unit>, lost N men, M left" from
  DisplayTotalMen, half a second after the last drop. Score (PercentRemaining) counts only routed/destroyed
  units (UnitSize x2 foot, x3 mounted/elephants), not men lost. NOT yet verified.
- Camera help left out (user, 2026-11-07): IDS_ONCE_ONLY_TUTORIAL_STRING73 ("Camera Controls:~~Scroll... Zoom...
  Rotate... Pitch/Tilt") is the only camera-only tutorial text; gather() drops a text whose first 40 chars match it.
