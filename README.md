# The Glorious Mod

Screen reader access for the Field of Glory games, starting with **Field of Glory II** (Steam). It speaks
through NVDA: the launcher, the menus, force selection, deployment and the battle map.

## Installing

1. Download `TheGloriousMod.exe` from the newest release on the
   [Releases page](https://github.com/Shehryar157/the-glorious-mod/releases).
2. Run it. Choose the game in the list (only Field of Glory II is available so far), then press **Install**.
   The installer finds the game through Steam; if it can't, use **Browse** to pick the game's folder.
3. Start the game as usual.

Run the installer again whenever you want to **Update**, **Reinstall** or **Uninstall**. Close the game and its
launcher first. The installer never overwrites a `version.dll` that another mod put in the game folder.

## Playing

In battle, press **F1** for the list of keys. In short: Q W E A D Z X C move the cursor over the map, S reads
the unit, Enter selects a unit or gives an order, N ends the turn (Enter to confirm), T reads the turn and
scores, O the briefing, R retreats (Enter to confirm), Page Up and Page Down scan through the units.

## How it works

`version.dll` is a small loader the game loads at startup; it loads `FoG2Access.dll`, the mod, which reads the
game's screens and battle from memory using the game's own symbol file and speaks through
`nvdaControllerClient32.dll` (NVDA's controller client, LGPL). Source: `src/loader`, `src/core`,
`src/installer`. Building needs [Zig](https://ziglang.org) (`python tools/build.py`); notes on the game's
internals are in `docs/findings`.
