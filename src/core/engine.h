// The game engine (Archon): addresses and class layouts looked up by name in
// the game's own symbol file (STUB_TILE_SHADER.pdb) when the mod starts, so a
// game patch doesn't break the mod unless the engine renames what we use.
// Also RTTI class checks and small typed readers for engine objects.
//
// Everything here reads live engine objects: call it on the game thread only.
#pragma once
#include <cstdint>
#include <string>

namespace eng {

// Offsets of the fields we read; -1 if the symbol file lacked one.
struct Layout {
    // CUIManager
    int ui_layers, ui_focus;
    int layer_size; // sizeof(TUILayer)
    // TUIScreen
    int screen_root, screen_name, screen_hidden, screen_flags;
    // CUIObject
    int obj_visible, obj_enabled, obj_box, obj_name, obj_tooltip, obj_children, obj_sibling;
    // CUIButton
    int button_string, button_is_checkbox, button_checked, button_greyed, button_indented;
    // CUIProgressBar
    int bar_progress;
    // CUIText
    int text_string;
    // CUIEditBox (a CUIObject, not a CUIText: its own CKString)
    int edit_string;
    // CUIListBox, CUIListBoxItem
    int list_items, list_selected, list_dropdown, item_size, item_string, item_data;
    // CUIObject
    int obj_id, obj_script;
    // Battle: CGame, CBattleDisplay, CSide, CUnit
    int game_battle, bd_my_side, bd_current_unit, bd_current_tile, bd_sides, side_size, side_units, side_unit_count;
    int bd_options, bd_option_count, option_size, option_object; // the orders offered on the hovered tile
    int bd_selected_tile, bd_deploy_mode;
    // Attributes: CSideData names (TAttrib[], count), CUnit::mAttrib, TSquadTemplate::mTemplateAttrib (TAttribSet: int value[] first)
    int sd_attrib_names, sd_attrib_count, attrib_size, unit_attrib, template_attrib;
    int unit_size, unit_tile, unit_type, unit_side, unit_dead, unit_removed, unit_facing, unit_force_visible;
    // CBattleMap, _TMapTile, CSideData, TSquadTemplate, TTerrainInfo
    int map_tiles, map_width, map_height, tile_size, tile_los, tile_unit, tile_terrain;
    int sd_templates, template_id, template_cost, terrain_size;
    // Scripts: CCScriptInstance -> CCScript -> CScriptHelper (variable names in 64 hash buckets, variables, structs)
    int inst_script, script_helper, helper_var_names, helper_vars, helper_structs;
    int varname_size, varname_index, var_size, var_data;
    int struct_size, struct_bytes, struct_name, struct_elements, element_size, element_offset, element_name;
};

extern Layout L;
extern uintptr_t ui_manager_ptr; // &gUIManager (a CUIManager*)
extern int trigger_slot;         // CUIButton::Trigger's vtable index
// Battle: &gGame, &gBattleMap, &gSideData (pointers to the objects); CCombat::mTerrain
// (TTerrainInfo[], name first) and &CCombat::mTerrainCount; CBattleDisplay::DoPick, ::HandleMouse, ::SelectUnit, ::DoActionOnCurrentUnit.
extern uintptr_t game_ptr, battle_map_ptr, side_data_ptr, terrain_table, terrain_count_ptr, fn_do_pick, fn_handle_mouse, fn_select_unit, fn_do_action;
// The mouse as the engine sees it: &gMouse (-> int x, y), &gUIBlockManager, CUIManager::GetControlUnderMouse(),
// CUIBlockManager::OverUIBlock(pos*, int), CBattleDisplay::GetMiniMapPos(pos*).
extern uintptr_t mouse_ptr, block_manager_ptr, fn_control_under_mouse, fn_over_block, fn_minimap_pos;
// CBattleMap::GetFixedPointHeight(fx, fy) (thiscall): ground height at a point, 256 units per square.
extern uintptr_t fn_fixed_height;

// Look everything up; false (and logged) if something is missing. Not on the
// game thread: it takes a few hundred milliseconds.
bool init(const std::wstring &dir);

enum Kind : unsigned { BUTTON = 1, TEXT = 2, LISTBOX = 4, EDITBOX = 8, BAR = 16 };
// Which UI classes an object is (or derives from), from its RTTI.
unsigned kinds(const void *obj);

template <typename T> inline T at(const void *base, int offset)
{
    return *(const T *)((const char *)base + offset);
}

// A CKString (uint16 *mText, int mSize, int mLen).
std::wstring kstring(const void *base, int offset);
std::wstring name(const void *obj);
// Game text without its markup (<colour=0x..>, <f2>, </f>...).
std::wstring plain(const std::wstring &s);
// A localized string by id ("IDS_UNITNAME48"); empty if the language files lack it.
std::wstring game_string(const char *id);
// Press a button the way a left click does.
void trigger(void *button);

// List boxes (drop-downs are list boxes with mIsDropdown).
int list_count(void *list);
std::wstring list_item(void *list, int index); // columns separated by ", "
unsigned list_item_data(void *list, int index); // what the screen's script stored with the entry
// Select an entry the way a left click on it does: set it, then tell the screen's script.
void list_select(void *list, int index);

// Edit boxes: put the typing cursor in one (CUIManager::RequestFocus, what a click on it does), or take it out.
void focus(void *edit);
void clear_focus();

// Call a function of an object's own script (CUIObject::mScript) the way the engine does:
// CCScriptInstance::GetFunction by name, then CallFunction with the arguments in the order the
// script declares its parameters. False if the object has no script or no such function.
bool call_script(void *obj, const char *function, const int *args, int count);

// Run fn(ctx) so that a fault inside the mod's own code (reading a game object that was just freed: the
// game rebuilds every screen when a module is switched, tester crash 2026-11-10 in strnlen from name())
// is caught instead of crashing the game. False if one was caught (logged). Faults in the game's code
// are left alone. guard_start / guard_stop: install / remove the handler (game thread only uses it).
bool guarded(void (*fn)(void *), void *ctx);
void guard_start();
void guard_stop();

// A global variable of an object's script, by name: its data (an int, or the first element of an
// array), or null. A struct type's member offset and the struct's size, by name: -1 if absent.
void *script_var(void *obj, const char *name);
int script_member(void *obj, const char *type, const char *member, int *type_size);

} // namespace eng
