// The battle map: grid cursor, unit and terrain speech, Enter as a left click
// on the cursor tile, and the scanner. Driven by the menu module's hook and
// timer on the game thread.
#pragma once
#include <windows.h>

void battle_start();  // after eng::init succeeded; hooks CBattleDisplay::DoPick
void battle_stop();   // on the game thread: unhooks
void battle_free();   // after the hook can no longer run: frees the trampoline
void battle_tick();   // every 16 ms
// True while a battle is up and no screen covers it: the battle keys are ours.
bool battle_owns();
// A battle HUD screen the menu module leaves alone while the battle owns the keys.
bool battle_hud(const char *screen_name);
// In battle, a screen with buttons that isn't the HUD or the tutorial's non-modal box (the unit
// information popup, messages): it covers the battle for the keys, though its flags don't say so.
bool battle_popup(char *screen);
// An entry of a list the game's script draws itself (the unit list: its data is the unit's index),
// as the script would draw it; empty if it isn't one.
std::wstring battle_list_entry(void *list, int index);
// A force selection row's cost and quantity ("Cost 72, Quantity 1 + 0/6"), or only the quantity;
// empty for other lists.
std::wstring battle_force_details(void *list, int index, bool quantity_only);
// Enter on such an entry: the unit list selects that unit on the map. True if it was one.
bool battle_list_pick(void *list, int index);
// Handle a key (WM_KEYDOWN or WM_SYSKEYDOWN); true if it was ours.
bool battle_key(WPARAM vk, bool act);
// A game key the battle doesn't need (camera and map-drawing keys): dropped while the battle owns the
// keys and nothing else took it.
bool battle_blocks(WPARAM vk);
