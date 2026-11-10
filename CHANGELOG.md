# Changes

Each release's section becomes its release notes on GitHub, and the installer shows them in its Changes box
before you update. Add a section here before running `python tools/build.py release X.Y.Z`; the release
refuses to go out without one.

## fog2-v0.1.3

- Fixed a crash when choosing Rise of Rome in the Quick Battles list, or launching it.
- Fixed a crash when switching game modules: if the game rebuilds its screens under the mod, the mod now carries on instead of crashing the game.
- Settings: Difficulty is one setting with its six levels (Centurion, Tribune, Legate, Governor, Emperor, Deity). It no longer jumps back to the top, and the level's name is no longer read before Mouse Button Mode.
- Settings: each option now reads the game's own description of it.
- Quick Battles: Pick your side, Pick enemy side and Pick conflict now take you straight into their list.
- Quick Battles: "Pick your side" is no longer said twice.
- The unit information popup (Ctrl+Enter) no longer starts with the previous unit's name.
- A changed text is no longer read twice after changing a setting.
- Deployment: squares outside the selected unit's area say "Cannot deploy here".
- Units waiting off the battlefield are spoken as "not available" and left out of the scanner.
- Scanner: categories say whose units they are ("You: Ancient British", "Enemy: Roman").
- Hills: the cursor says "(Hill)" and the square's height.
- R retreats from the battle (asks first), O reads the briefing.
- End of battle: Final Losses is a table; the battlefield review keeps the map keys and N ends it.
- Campaigns: each campaign is its own menu item with its description; locked ones say which expansion they need.
- The installer shows what's new before you update.

## fog2-v0.1.1

- The installer is the only download; the mod's files are inside it.

## fog2-v0.1.0

- First release.
