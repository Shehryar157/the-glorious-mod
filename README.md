# The Glorious Mod

Screen reader access for the Field of Glory games, starting with **Field of Glory II** (Steam). The game speaks
through **NVDA**: the launcher, every menu, force selection, deployment, the battle map, combat results and the
end-of-battle screens.

## Installation

1. Download **TheGloriousMod.exe** from the newest release on the
   [Releases page](https://github.com/Shehryar157/the-glorious-mod/releases). That one file is all you need.
2. Close Field of Glory II and its launcher, then run TheGloriousMod.exe.
3. In the list, choose **Field of Glory II**. (Field of Glory II: Medieval, Field of Glory: Empires and Field of
   Glory: Kingdoms are listed as not available yet.)
4. Press **Install**. The installer finds the game through Steam; if it can't, press **Browse** and pick the
   game's folder. A progress bar follows the download, and a message tells you when it's done.
5. Start the game as usual, with NVDA running.

Run TheGloriousMod.exe again whenever you like:

- **Update** downloads the newest release and installs it (it tells you if you're already up to date). Before
  the buttons, the **Changes** box lists what's new in every version you'd get; it only appears once the mod is
  installed.
- **Reinstall** puts the newest release in again, over what's there.
- **Uninstall** removes every file the mod added.

The installer needs no internet for a first install (the mod is inside it), never touches a `version.dll` that
another mod put in the game folder, and asks you to close the game first if it's running. If the game is under
Program Files, run the installer as administrator.

## What the mod does

The mod makes these parts of the game accessible:

- The launcher
- The main menu, settings, and loading and saving games
- Tutorials, battles and campaigns, including each campaign's description
- Force selection, where you buy your army
- Deployment
- The battle itself, played on the battle grid
- The end of the battle: the result, the losses and the battlefield review

## What it cannot yet do

- Field of Glory II: Medieval, Field of Glory: Empires and Field of Glory: Kingdoms aren't supported yet.
- The scenario editor and the multiplayer lobby haven't been made accessible.

## Keys

In menus, the arrows, Tab, Home, End, Enter, Space and Escape work as you'd expect. These are the mod's keys in
battle (press **F1** in battle to hear them):

| Key | What it does |
| --- | --- |
| Q, W, E, A, D, Z, X, C | Move the cursor (W north, X south, A west, D east, Q E Z C the diagonals) |
| Ctrl + a cursor key | Move the cursor with the game's detailed tooltips |
| Alt + a cursor key | Order the selected unit to the next square that way; it stays selected |
| Enter | Select the unit on the cursor, or give the order there (several orders: a list to choose from) |
| Alt + Enter | Undo the unit's move |
| Ctrl + Enter | The unit's information |
| Escape | Deselect the unit |
| S | The unit on the cursor: name, AP, men, morale, facing and the game's panel |
| T | Turn and scores |
| O | Battle name, date and briefing |
| N | End the turn: the game asks to confirm; Enter or N confirms, Escape cancels. In the battlefield review: end the review |
| R | Retreat from the battle: Enter or R confirms, Escape cancels |
| Page Up, Page Down | Scanner: previous or next unit type |
| Alt + Page Up, Alt + Page Down | Scanner: previous or next unit of the same type |
| Ctrl + Page Up, Ctrl + Page Down | Scanner: switch category (your units, enemy units, generals) |
| Home | Move the cursor to the unit the scanner is on |
| End | Where the scanner's unit is from the cursor |
| [ and ] | Previous or next message category |
| Comma, Period | Older or newer message |
| Shift + [ or ] | First or last message category |
| Shift + Comma or Period | Oldest or newest message |
| L | Unit list; Enter selects the unit |
| K | Casualties |
| Tab | The game's next unit that hasn't moved |
| B | The game's next unit that can shoot |
| Space | Close a message or proceed |
| F1 | This key list |
| F2 | Screenshot |

On the force selection list, **Right** buys one of the unit type and **Left** sells one. In the Final Losses
table, **Up/Down** move between rows, **Left/Right** between columns, **Home/End** to a row's first or last
column.

## Scanner

The scanner finds units without moving the cursor over the whole map. It has three categories, which
**Ctrl + Page Up** and **Ctrl + Page Down** switch between:

- **You:** your army's units, for example "You: Ancient British".
- **Enemy:** the enemy's units you can see, for example "Enemy: Roman". Units out of your line of sight aren't
  listed.
- **Generals:** every general you can see, both sides, each one on its own.

Within a category, units are grouped by type. **Page Up** and **Page Down** go to the previous or next type,
**Alt + Page Up** and **Alt + Page Down** through the units of that type. Units are sorted nearest first from the
cursor, and the order is worked out again on every key press, so after you move the cursor the next unit is
the nearest to where you are now. Each unit is read with its position in its type, where it is from the cursor
("3 north, 2 east") and the type's position in the category: "Warband 2 of 5, 3 north, 2 east, 1 of 4".
**Home** moves the cursor onto the unit; **End** repeats where it is.

## How the mod works

For the technically minded:

- **Getting in:** Field of Glory II loads `version.dll` from its own folder at startup, before the Windows one.
  The mod's `version.dll` is a proxy: it passes the real functions through to Windows and loads the mod,
  `FoG2Access.dll`, into both the launcher (`autorun.exe`) and the game (`FIELDOFGLORY2.EXE`). It reloads the mod
  whenever the DLL changes, so a new build takes effect without restarting the game.
- **Reading the game:** the game ships with its symbol file. At startup the mod uses it, through the game's own
  `dbghelp.dll`, to look up the addresses of the engine's globals and functions and the layout of its classes
  (the UI manager, screens, buttons, list boxes, the battle display, units, the map). Nothing is hard-coded to
  one game version, so patches don't break it unless the engine renames what it uses. The game's scripts' own
  variables (force selection, the end-turn prompt) are found by name in the script engine.
- **Menus:** every 16 ms, on the game's own thread, the mod walks the screens that are up and turns their objects
  into controls by type and position. It presses buttons and selects list entries the way the engine does for a
  click, so the game's scripts react as they would to the mouse.
- **The battle map:** the mod hooks the function that works out which square the mouse is over, and puts the
  keyboard cursor's square there. The game then shows its own tooltips and offers its own orders for that
  square, which the mod reads, and an order is given by calling the same function a click calls. No mouse,
  window size or screen resolution is involved, so it works the same whatever your display.
- **Speech:** through NVDA's controller client (`nvdaControllerClient32.dll`, LGPL; licence in `native/`).
- **The installer:** a plain Win32 program, so every screen reader reads it. The mod's files are stored inside
  the exe; Install and Update fetch the newest release's exe from GitHub and take the files out of it.

Building needs [Zig](https://ziglang.org) and Python: `python tools/build.py` builds the mod (and copies it into
the game, which reloads it), `python tools/build.py release X.Y.Z` publishes a release. Notes on the game's
internals are in `docs/findings`.
