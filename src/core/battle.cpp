// The battle map.
//
// The battle is up while gGame->mBattle and gBattleMap exist and BATTLECOMMON
// is on screen with no covering screen (flag 1) above it; then the battle keys
// are ours and the menu module only reads the other screens (tutorial popups).
//
// The map (CBattleMap) is mWidth x mHeight _TMapTile; each tile has its unit
// (CUnit*), terrain (index into CCombat::mTerrain, named IDS_TERRAIN_<name> in
// the language files) and losFlags (bit side*8: that side sees the tile). The
// units are CBattleDisplay::mSides[side].mUnit[0..mUnitCount); a unit's display
// name is IDS_UNITNAME<id>, id being its squad template's mID (Squads.csv ID).
// Enemy units on tiles our side doesn't see are left out everywhere.
//
// Grid: a cursor of our own. CBattleDisplay::DoPick sets mCurrentTile (the
// hovered tile) each frame; we hook it and put the cursor tile there after it
// runs, so the game's hover panels describe the cursor tile and a left click
// (Enter: the battle display's mouse handler called directly, no real mouse) acts on it.
// Moving speaks the unit, terrain and coordinates, then the orders offered there
// for the selected unit and any text the game changes within half a second.
//
// Keys: Q W E / A D / Z X C move (north = -y), S reads the unit with the
// game's unit panel, Enter clicks the tile (right click when the game offers an
// order there for the selected unit), Ctrl+Enter is the game's Ctrl+click (unit
// information), Ctrl+direction moves with Ctrl held (the game's detailed
// tooltips), Escape drops the selection. An order given drops the selection;
// Enter never gives Undo, Alt+Enter does. Spoken as they happen and kept in
// buffers by category: combat log lines, the turn changing hands, enemy units
// moving or coming into view, morale changes, deployment messages.
// [ ] switch buffer category, comma / period older / newer, Shift: to the end. Alt+direction: the selected unit's
// order on the next square that way (no Enter needed). T turn and scores,
// R briefing, N End Turn (again: Confirm). Scanner: Ctrl+PgUp/PgDn
// switch side, PgUp/PgDn unit types, Alt+PgUp/PgDn units of the same type,
// Home moves the cursor to the unit, End says where it is from the cursor.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "battle.h"
#include "engine.h"
#include "speech.h"

using eng::at;
using eng::L;

namespace {

const BYTE PROLOGUE[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00}; // push ebp; mov ebp,esp; sub esp,0xB0
BYTE *trampoline;
bool hooked, owns;
char *seen_map; // the map the cursor belongs to; a new battle resets it
int cur_x = -1, cur_y = -1;
std::vector<std::pair<void *, std::wstring>> texts_before; // hover texts when the cursor moved
DWORD watch_until;
unsigned picks, picks_at_move; // DoPick runs; the game fills the tile's orders a frame after it hovers it
std::wstring orders_said;
int selected = -1;
int spot_x = -1, spot_y; // where the engine's mouse is put: on the open map
int end_turn_speech;     // ticks until the end-turn button's new state is spoken
int order_menu = -1;     // the order chosen in the open order list; -1: closed
bool step_pending;       // Alt+direction: give the order once the game has the new tile's orders
bool from_step;          // the order being given came from Alt+direction: the unit stays selected
std::vector<int> menu_orders; // the orders in the open list (Undo left out: Alt+Enter)
void *log_list;          // the combat log (BATTLECOMBATLOG) and how many of its lines were spoken
int log_said;
// The buffers (user, 2026-11-01, like MBAccess's): everything announced on its own, by category;
// [ ] switch category, comma / period read older / newer; with Shift: first / last category, oldest / newest.
enum Category { ALL, COMBAT, MORALE, MOVES, TURNS, DEPLOY, CATEGORIES };
const wchar_t *const category_names[CATEGORIES] = {L"All", L"Combat", L"Morale", L"Movement", L"Turns", L"Deployment"};
struct Entry {
    int category;
    std::wstring text;
};
std::vector<Entry> history;
int buffer_category, buffer_index = -1; // index into the category's entries; -1: the newest
bool enemy_turn;         // EnemyTurnImage was visible
std::wstring deploy_said; // the deployment message last spoken
bool deploy_checked;      // the cursor square's "Cannot deploy here" was decided
bool retreat_prompt;      // R asked; Enter / R retreats, Escape cancels
bool end_prompt;          // the End Turn prompt (Confirm) is open: Enter / N / Escape act on it
DWORD last_user_key;      // when the user last pressed a key the battle or the unit list acted on
// Per unit, to speak what changes: where it was last announced, where it was last tick, how long it
// has stood still, whether we could see it, its morale state.
struct Track {
    int said_x, said_y, x, y, still;
    bool seen;
    int morale;
    int men, lost, quiet; // men now, men lost not yet spoken, ticks since the last loss
};
std::unordered_map<char *, Track> tracks;
bool tracks_ready; // CBattleDisplay::mCurrentUnit last seen

// Scanner position: category (0 our side, 1 the other), unit type, the unit.
int scan_cat;
std::wstring scan_name;
char *scan_unit;

struct Map {
    char *tiles;
    int w, h;
};

// Speak something the battle did and keep it in the buffers.
void note(int category, const std::wstring &text, bool say = true)
{
    if (text.empty())
        return;
    if (say)
        speak(text, false);
    history.push_back({category, text});
    if (history.size() > 1000)
        history.erase(history.begin(), history.begin() + 200);
}

std::vector<const Entry *> buffer_entries(int category)
{
    std::vector<const Entry *> out;
    for (const Entry &e : history)
        if (category == ALL || e.category == category)
            out.push_back(&e);
    return out;
}

void speak_buffer(bool with_category)
{
    std::vector<const Entry *> es = buffer_entries(buffer_category);
    std::wstring s = with_category ? std::wstring(category_names[buffer_category]) + L", " : L"";
    if (es.empty()) {
        speak(s + L"empty");
        return;
    }
    buffer_index = std::max(0, std::min((int)es.size() - 1, buffer_index));
    speak(s + es[buffer_index]->text + L", " + std::to_wstring(buffer_index + 1) + L" of " + std::to_wstring(es.size()));
}

// [ ] , . (Shift: to the end that way).
void buffer_step(WPARAM vk, bool shift)
{
    int n = (int)buffer_entries(buffer_category).size();
    if (buffer_index < 0 || buffer_index >= n)
        buffer_index = n - 1;
    if (vk == VK_OEM_4 || vk == VK_OEM_6) {
        int step = vk == VK_OEM_4 ? -1 : 1;
        buffer_category = shift ? (step < 0 ? 0 : CATEGORIES - 1) : (buffer_category + step + CATEGORIES) % CATEGORIES;
        buffer_index = (int)buffer_entries(buffer_category).size() - 1; // its newest
        speak_buffer(true);
        return;
    }
    if (vk == VK_OEM_COMMA)
        buffer_index = shift ? 0 : buffer_index - 1;
    else
        buffer_index = shift ? n - 1 : buffer_index + 1;
    speak_buffer(false);
}

struct Unit {
    char *obj;
    int side, x, y;
    std::wstring name;
};

char *battle()
{
    char *game = *(char **)eng::game_ptr;
    return game ? at<char *>(game, L.game_battle) : nullptr;
}

bool get_map(Map &m)
{
    char *map = *(char **)eng::battle_map_ptr;
    if (!map)
        return false;
    m = {at<char *>(map, L.map_tiles), at<int>(map, L.map_width), at<int>(map, L.map_height)};
    return m.tiles && m.w > 0 && m.h > 0;
}

char *tile(const Map &m, int x, int y) { return m.tiles + (y * m.w + x) * L.tile_size; }
int my_side() { return (int)at<unsigned>(battle(), L.bd_my_side); }

// --- reading ---

std::wstring trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i]))
        i++;
    return s.substr(i);
}

std::wstring unit_name(char *unit)
{
    static std::unordered_map<int, std::wstring> names; // by template mID
    unsigned type = at<unsigned>(unit, L.unit_type);
    char *side_data = *(char **)eng::side_data_ptr;
    char *templ = side_data && type < 1280 ? ((char **)(side_data + L.sd_templates))[type] : nullptr;
    if (!templ)
        return {};
    int id = at<int>(templ, L.template_id);
    auto hit = names.find(id);
    if (hit != names.end())
        return hit->second;
    std::string key = "IDS_UNITNAME" + std::to_string(id);
    std::wstring n = eng::game_string(key.c_str());
    if (n.empty())
        n = std::to_wstring(id);
    return names[id] = n;
}

std::wstring terrain_name(int t)
{
    if (t < 0 || t >= at<int>((void *)eng::terrain_count_ptr, 0))
        return {};
    const char *n = (const char *)(eng::terrain_table + t * L.terrain_size);
    std::string key = "IDS_TERRAIN_" + std::string(n, strnlen(n, 32));
    std::wstring s = eng::game_string(key.c_str());
    return s.empty() ? std::wstring(n, n + strnlen(n, 32)) : s;
}

char *screen(const char *name)
{
    char *ui = *(char **)eng::ui_manager_ptr;
    for (int i = 0; ui && i < 24; i++) {
        char *s = *(char **)(ui + L.ui_layers + i * L.layer_size);
        if (s && !lstrcmpiA(s + L.screen_name, name))
            return s;
    }
    return nullptr;
}

void *find(void *obj, const wchar_t *name)
{
    for (int budget = 2000; obj && budget > 0; obj = at<void *>(obj, L.obj_sibling), budget--) {
        if (eng::name(obj) == name)
            return obj;
        if (void *inner = find(at<void *>(obj, L.obj_children), name))
            return inner;
    }
    return nullptr;
}

// Visible texts under obj (and its siblings when siblings is true).
void texts(void *obj, std::vector<std::pair<void *, std::wstring>> &out, bool siblings)
{
    for (int budget = 3000; obj && budget > 0; obj = at<void *>(obj, L.obj_sibling), budget--) {
        if (at<bool>(obj, L.obj_visible)) {
            if ((eng::kinds(obj) & (eng::TEXT | eng::BUTTON | eng::LISTBOX)) == eng::TEXT) {
                std::wstring t = trim(eng::plain(eng::kstring(obj, L.text_string)));
                if (!t.empty())
                    out.push_back({obj, t});
            }
            texts(at<void *>(obj, L.obj_children), out, true);
        }
        if (!siblings)
            break;
    }
}

// Visible texts outside the battle HUD (its panels are read with S, T and R): tooltips and popups.
void all_texts(std::vector<std::pair<void *, std::wstring>> &out)
{
    out.clear();
    char *ui = *(char **)eng::ui_manager_ptr;
    for (int i = 0; ui && i < 24; i++) {
        char *s = *(char **)(ui + L.ui_layers + i * L.layer_size);
        if (s && !at<bool>(s, L.screen_hidden) && !battle_hud(s + L.screen_name))
            texts(at<void *>(s, L.screen_root), out, true);
    }
}

std::wstring joined(void *obj)
{
    std::vector<std::pair<void *, std::wstring>> t;
    if (obj)
        texts(obj, t, false);
    std::wstring s;
    for (auto &p : t)
        s += (s.empty() ? L"" : L", ") + p.second;
    return s;
}

// The side's name as the score panel shows it ("Roman").
std::wstring faction(int side)
{
    char *s = screen("BATTLECOMMON");
    void *o = s ? find(at<void *>(s, L.screen_root), side ? L"BATTLE_SCORESIDE1NAME" : L"BATTLE_SCORESIDE0NAME") : nullptr;
    std::wstring n = o ? trim(eng::plain(eng::kstring(o, L.text_string))) : L"";
    return n.empty() ? std::to_wstring(side + 1) : n;
}

// Our side sees the square, as _TMapTile::GetLOS(side) works it out (disassembled live, 2026-11-07): a
// unit there with mForceVisible counts as seen, else the side's whole byte of losFlags must be non-zero
// (the mod tested only its lowest bit and called seen squares, and the enemies on them, out of sight).
bool seen(const Map &m, int x, int y)
{
    char *t = tile(m, x, y);
    char *u = at<char *>(t, L.tile_unit);
    if (u && L.unit_force_visible >= 0 && at<bool>(u, L.unit_force_visible))
        return true;
    return ((at<unsigned>(t, L.tile_los) >> (my_side() * 8)) & 0xff) != 0;
}

bool visible(const Map &m, char *unit)
{
    if (at<bool>(unit, L.unit_dead) || at<bool>(unit, L.unit_removed))
        return false;
    int side = (int)at<unsigned>(unit, L.unit_side), x = at<int>(unit, L.unit_tile), y = at<int>(unit, L.unit_tile + 4);
    if (x < 0 || y < 0 || x >= m.w || y >= m.h)
        return false;
    int me = my_side();
    return side == me || seen(m, x, y);
}

// The off-map predeployment area (ForceSelectionTools.bsf): units waiting there sit in the map's border
// rows, unseen even by their own side, and the game won't select them (tried 2026-11-10). Spoken with
// "not available" (user, 2026-11-10: mark them rather than hide them).
bool parked(const Map &m, char *unit)
{
    int x = at<int>(unit, L.unit_tile), y = at<int>(unit, L.unit_tile + 4);
    bool border = x == 0 || y == 0 || x == m.w - 1 || y == m.h - 1;
    return border && (int)at<unsigned>(unit, L.unit_side) == my_side() && !seen(m, x, y);
}

char *unit_at(const Map &m, int x, int y)
{
    char *u = at<char *>(tile(m, x, y), L.tile_unit);
    return u && visible(m, u) ? u : nullptr;
}

char *unit_by_index(int index)
{
    int side = index / 1000, i = index % 1000;
    if (index < 0 || side > 1)
        return nullptr;
    char *s = battle() + L.bd_sides + side * L.side_size;
    return i < (int)at<unsigned>(s, L.side_unit_count) ? s + L.side_units + i * L.unit_size : nullptr;
}

bool is_selected(char *unit) { return unit && unit == unit_by_index(at<int>(battle(), L.bd_current_unit)); }

int attrib_index(const char *name)
{
    char *sd = *(char **)eng::side_data_ptr;
    unsigned n = sd ? at<unsigned>(sd, L.sd_attrib_count) : 0;
    for (unsigned i = 0; i < n && i < 192; i++)
        if (!lstrcmpiA(sd + L.sd_attrib_names + i * L.attrib_size, name))
            return (int)i;
    return -1;
}

// "600/720" when the unit has lost men, counted like the game's panel (UITools.BSF
// PrintStrengthString: TotalMen against StartingStrength = base TotalMen * UnitSize / base UnitSize).
std::wstring strength(char *unit)
{
    static int men = -1, size = -1;
    if (men < 0 || size < 0) {
        men = attrib_index("TotalMen");
        size = attrib_index("UnitSize");
        if (men < 0 || size < 0)
            return {};
    }
    char *side_data = *(char **)eng::side_data_ptr;
    unsigned type = at<unsigned>(unit, L.unit_type);
    char *templ = type < 1280 ? ((char **)(side_data + L.sd_templates))[type] : nullptr;
    if (!templ)
        return {};
    const int *now = (const int *)(unit + L.unit_attrib), *base = (const int *)(templ + L.template_attrib);
    if (base[size] <= 0)
        return {};
    int start = base[men] * now[size] / base[size];
    if (now[men] >= start)
        return {};
    return std::to_wstring(now[men]) + L"/" + std::to_wstring(start);
}

// CUnit::mUnitFacing: degrees, 0 = +y (our south), 90 = +x (east), 180 = north, 270 = west
// (gIVectorFromDirectionTable: 0 -> (0, 64), 90 -> (64, 0), 135 -> (45, -45)).
std::wstring facing(char *unit)
{
    static const wchar_t *const names[] = {L"south", L"southeast", L"east", L"northeast",
                                           L"north", L"northwest", L"west", L"southwest"};
    int f = at<uint16_t>(unit, L.unit_facing) % 360;
    return std::wstring(L"facing ") + names[(f + 22) / 45 % 8];
}

// A general with the unit (UnitList.bsf: attribute General > -1): its rank in the game's words,
// IDS_GENERAL_TYPE<GeneralType> ("Sub-General", "C-in-C", "Ally General"); empty without one.
std::wstring general(char *unit)
{
    static int has = -2, type = -2;
    if (has == -2) {
        has = attrib_index("General");
        type = attrib_index("GeneralType");
    }
    const int *a = (const int *)(unit + L.unit_attrib);
    if (has < 0 || a[has] <= -1)
        return {};
    std::wstring s = type >= 0 ? eng::game_string(("IDS_GENERAL_TYPE" + std::to_string(a[type])).c_str()) : L"";
    return s.empty() ? eng::game_string("IDS_TT_GENERAL") : s;
}

// The unit's name with its general, if it has one (user, 2026-11-07): "Warband (Loose Order), has
// general"; with rank (S, the Generals scanner category) "..., has general, C-in-C".
std::wstring name_and_general(char *unit, bool rank = false)
{
    std::wstring g = general(unit);
    return unit_name(unit) + (g.empty() ? L"" : rank ? L", has general, " + g : L", has general");
}

bool parked(char *unit)
{
    Map m;
    return get_map(m) && parked(m, unit);
}

std::wstring describe(char *unit)
{
    std::wstring s = name_and_general(unit);
    if (parked(unit))
        return s + L", not available"; // waiting off the battlefield: facing, command range don't apply
    std::wstring str = strength(unit);
    if (!str.empty())
        s += L", " + str;
    s += L", " + facing(unit);
    // The game's 3 key draws command ranges: say it of our units out of range (GeneralInCommandRange -1:
    // no general in range; such units lose their free 45 degree turn, IDS_LTIP37).
    static int command = -2;
    if (command == -2)
        command = attrib_index("GeneralInCommandRange");
    if (command >= 0 && (int)at<unsigned>(unit, L.unit_side) == my_side() &&
        ((const int *)(unit + L.unit_attrib))[command] == -1)
        s += L", out of command range";
    int side = (int)at<unsigned>(unit, L.unit_side);
    if (side != my_side())
        s += L", " + faction(side);
    if (is_selected(unit))
        s += L", selected";
    return s;
}

std::wstring offset(int x, int y)
{
    int dx = x - cur_x, dy = y - cur_y;
    std::wstring s;
    if (dy)
        s = std::to_wstring(dy < 0 ? -dy : dy) + (dy < 0 ? L" north" : L" south");
    if (dx)
        s += (s.empty() ? L"" : L", ") + std::to_wstring(dx < 0 ? -dx : dx) + (dx < 0 ? L" west" : L" east");
    return s.empty() ? L"here" : s;
}

// --- grid ---

// The scripts' GetTileHeight (CBattleScriptHandler::CallCustomFunction, disassembled 2026-11-09):
// GetFixedPointHeight(x*256+128, y*256+128) * 100 >> 8, on the map gCombat points to (= gBattleMap).
// Hills are 100, 200 or 300; the corner heights of the first tutorial's hills were 512 -> 200.
int tile_height(int x, int y)
{
    typedef int(__thiscall * Height_t)(void *map, int fx, int fy);
    char *map = *(char **)eng::battle_map_ptr;
    if (!map || !eng::fn_fixed_height)
        return 0;
    return (((Height_t)eng::fn_fixed_height)(map, x * 256 + 128, y * 256 + 128) * 100) >> 8;
}

void speak_tile(const Map &m)
{
    std::wstring s;
    if (char *u = unit_at(m, cur_x, cur_y))
        s = describe(u) + L", ";
    s += terrain_name(at<uint8_t>(tile(m, cur_x, cur_y), L.tile_terrain));
    // As the game's terrain tooltip (Helpers.bsf HELPER_TERRAIN_TOOLTIP): "Open ground (Hill)" and
    // "Height: 200" (user, 2026-11-09: hills weren't spoken).
    if (int height = tile_height(cur_x, cur_y)) {
        static std::wstring hill = eng::game_string("IDS_TT_HILLS"), label = eng::game_string("IDS_TT_TILEHEIGHT");
        if (height > 0)
            s += L" " + trim(hill.empty() ? L"(Hill)" : hill);
        s += L", " + trim(label.empty() ? L"Height:" : label) + L" " + std::to_wstring(height);
    }
    // The game's 2 key draws line of sight: say it of squares our side doesn't see.
    if (!seen(m, cur_x, cur_y))
        s += L", out of sight";
    s += L", " + std::to_wstring(cur_x) + L", " + std::to_wstring(cur_y);
    speak(s);
    all_texts(texts_before);
    watch_until = GetTickCount() + 500;
    picks_at_move = picks;
    orders_said.clear();
    deploy_checked = false;
}

void place_cursor(const Map &m)
{
    // The selected unit, else our first unit, else the middle.
    char *u = unit_by_index(at<int>(battle(), L.bd_current_unit));
    char *ours = battle() + L.bd_sides + my_side() * L.side_size;
    for (unsigned i = 0; (!u || !visible(m, u)) && i < at<unsigned>(ours, L.side_unit_count); i++)
        u = ours + L.side_units + i * L.unit_size;
    if (u && visible(m, u)) {
        cur_x = at<int>(u, L.unit_tile);
        cur_y = at<int>(u, L.unit_tile + 4);
    } else {
        cur_x = m.w / 2;
        cur_y = m.h / 2;
    }
}

void move(int dx, int dy)
{
    Map m;
    if (!get_map(m))
        return;
    int x = cur_x + dx, y = cur_y + dy;
    if (x < 0 || y < 0 || x >= m.w || y >= m.h) {
        speak(L"edge");
        return;
    }
    cur_x = x;
    cur_y = y;
    speak_tile(m);
}

// A unit's state as the unit list shows it: "AP: n", men now/at the start, morale state.
struct Stats { std::wstring ap, strength, morale; };
Stats unit_stats(char *u);

// S (user, 2026-11-07): AP first, then the unit, its men and morale, then the game's own panel.
void read_unit()
{
    Map m;
    if (!get_map(m))
        return;
    char *u = unit_at(m, cur_x, cur_y);
    if (!u) {
        speak(L"empty");
        return;
    }
    Stats st = unit_stats(u);
    // Name first, then AP (user, 2026-11-09; was AP first since 2026-11-07).
    std::wstring s = name_and_general(u, true);
    if (!st.ap.empty())
        s += L", " + st.ap;
    if (!st.strength.empty())
        s += L", " + st.strength;
    if (!st.morale.empty())
        s += L", " + st.morale;
    std::wstring rest = describe(u); // name, [strength,] facing, command range, faction, selected
    size_t facing_at = rest.find(L", facing ");
    if (facing_at != std::wstring::npos)
        s += rest.substr(facing_at);
    // The game's own panel: the selected unit's, or the hovered (targeted) one's.
    char *panel = screen("BATTLEADVANCEDPANEL");
    void *root = panel ? at<void *>(panel, L.screen_root) : nullptr;
    std::wstring more = joined(root ? find(root, is_selected(u) ? L"BATTLE_SELECTEDBOX" : L"BATTLE_TARGETEDBOX") : nullptr);
    if (!more.empty())
        s += L". " + more;
    speak(s);
}

// The orders the game offers for the selected unit on the hovered tile (TOptionEntry: the order's icon,
// whose tooltip is its text, "Move Unit 8/10"), joined.
// The icon's tooltip is what a sighted player reads over it, including why the order can't be given
// ("Too few AP to turn"); a greyed-out (disabled) icon also says "unavailable".
int order_count() { return (int)std::min(at<unsigned>(battle(), L.bd_option_count), 16u); }

void *order_icon(int i) { return at<void *>(battle() + L.bd_options + i * L.option_size, L.option_object); }

bool order_enabled(int i)
{
    void *icon = order_icon(i);
    return icon && at<bool>(icon, L.obj_enabled);
}

std::wstring order_text(int i)
{
    void *icon = order_icon(i);
    if (!icon)
        return {};
    // The tooltip's lines (CKString mToolTipString[3], each may hold several lines). A line telling a
    // mouse player to click the order's icon ("Your unit can shoot at this enemy unit. <click> on the
    // Shoot icon if you want it to do so.") gives the order's name and is otherwise dropped: the name
    // comes first, then the facts (user, 2026-11-07): "Shoot, Half Arc of Fire, Casualties: 4 - 16".
    // Outside tutorial mode the game writes "Turn and Shoot:" as a first line instead (TurnAndShoot.BSF);
    // in the tutorial text the name may start on a new line ("on the~Turn and Shoot icon").
    std::wstring name, rest;
    for (int k = 0; k < 3; k++) {
        std::wstring text = eng::plain(eng::kstring(icon, L.obj_tooltip + k * 12));
        size_t ic = name.empty() ? text.find(L" icon") : std::wstring::npos;
        size_t the = ic == std::wstring::npos ? std::wstring::npos : text.rfind(L"the", ic);
        if (the != std::wstring::npos && the + 4 <= ic && (text[the + 3] == L' ' || text[the + 3] == L'\n')) {
            name = text.substr(the + 4, ic - the - 4);
            std::replace(name.begin(), name.end(), L'\n', L' ');
            name = trim(name);
            size_t from = text.rfind(L'\n', the), to = text.find(L'\n', ic);
            from = from == std::wstring::npos ? 0 : from + 1;
            text.erase(from, to == std::wstring::npos ? std::wstring::npos : to - from); // the whole click instruction
        }
        for (size_t a = 0; a <= text.size();) {
            size_t b = std::min(text.find(L'\n', a), text.size());
            std::wstring line = trim(text.substr(a, b - a));
            a = b + 1;
            if (name.empty() && rest.empty() && line.size() > 1 && line.back() == L':') {
                line.pop_back();
                name = line;
                continue;
            }
            if (!line.empty() && line.back() == L'.')
                line.pop_back();
            if (!line.empty())
                rest += (rest.empty() ? L"" : L", ") + line;
        }
    }
    // Outside the tutorial the charge tooltip has no name, only "Impact: Win 83%, ... Melee: ...":
    // put the game's word for it first (IDS_TT_CHARGE; another file uses that id for a long sentence).
    static std::wstring impact = eng::game_string("IDS_TT_IMPACT"), charge = eng::game_string("IDS_TT_CHARGE");
    // (The detailed version, Ctrl held, starts "Impact, Points of Advantage...".)
    if (name.empty() && !impact.empty() && rest.size() > impact.size() && rest.compare(0, impact.size(), impact) == 0 &&
        (rest[impact.size()] == L':' || rest[impact.size()] == L','))
        name = !charge.empty() && charge.size() <= 20 ? charge : L"Charge";
    std::wstring t = name.empty() ? rest : rest.empty() ? name : name + L", " + rest;
    return at<bool>(icon, L.obj_enabled) ? t : t + L", unavailable";
}

// Undo (Undo.BSF CHECK_ALL_UNDO, tooltip IDS_TT_UNDO): offered on the square a unit came from.
bool is_undo(int i)
{
    static std::wstring word;
    if (word.empty())
        word = eng::game_string("IDS_TT_UNDO");
    void *icon = order_icon(i);
    std::wstring t = icon ? trim(eng::plain(eng::kstring(icon, L.obj_tooltip))) : L"";
    return !word.empty() && t.compare(0, word.size(), word) == 0;
}

// An icon without a tooltip (seen in deployment, 2026-11-09, between "Deploy here" and "Turn Whole
// Command"): nothing to say or choose.
bool order_blank(int i)
{
    std::wstring t = order_text(i);
    return t.empty() || t == L", unavailable";
}

std::wstring orders()
{
    std::wstring s;
    for (int i = 0; i < order_count(); i++)
        if (!order_blank(i))
            s += (s.empty() ? L"" : L". ") + order_text(i);
    return s;
}

// The game works out the hovered tile's orders (CBattleDisplay::GameUpdate) only while its mouse
// (gMouse) is over the open map: the battle display is the control under it, no map icon (UI block)
// and not the minimap. So we keep gMouse on such a spot, found with the engine's own tests at
// fractions of the battle display's box: no screen size or window is assumed, and the real
// mouse can be anywhere.
bool open_map_at(char *bd, int x, int y)
{
    typedef void *(__thiscall * Under_t)(void *ui);
    typedef bool(__thiscall * OverBlock_t)(void *blocks, int *pos, int flag);
    typedef bool(__thiscall * MiniMap_t)(void *bd, int *pos);
    int *mouse = *(int **)eng::mouse_ptr;
    mouse[0] = x;
    mouse[1] = y;
    if (((Under_t)eng::fn_control_under_mouse)(*(void **)eng::ui_manager_ptr) != bd)
        return false;
    if (((OverBlock_t)eng::fn_over_block)(*(void **)eng::block_manager_ptr, mouse, 0))
        return false;
    int pos[2] = {x, y}; // GetMiniMapPos may turn it into a map position
    return !((MiniMap_t)eng::fn_minimap_pos)(bd, pos);
}

void aim_mouse()
{
    char *bd = battle();
    int *mouse = *(int **)eng::mouse_ptr;
    if (!mouse)
        return;
    if (spot_x >= 0 && open_map_at(bd, spot_x, spot_y))
        return;
    int old_x = mouse[0], old_y = mouse[1];
    RECT box = at<RECT>(bd, L.obj_box);
    static const int spots[] = {50, 40, 60, 30, 70, 20, 80, 10, 90};
    for (int fy : spots)
        for (int fx : spots) {
            int x = box.left + (box.right - box.left) * fx / 100, y = box.top + (box.bottom - box.top) * fy / 100;
            if (open_map_at(bd, x, y)) {
                spot_x = x;
                spot_y = y;
                return;
            }
        }
    spot_x = -1;
    mouse[0] = old_x;
    mouse[1] = old_y;
}

// A click on the cursor tile through CBattleDisplay::HandleMouse (msg, x, y, wParam, unused), the handler
// the UI manager calls with the window's mouse messages. It acts on mCurrentTile: a left click selects our
// unit there (or clears the selection), a right click carries out the order offered there (with the game's
// default control mode). x, y only test the map's clickable icons (HandleMouseClickOnUIBlock), so a spot
// off the screen keeps the click on the tile, whatever the window's size, mode or resolution.
void click()
{
    typedef int(__thiscall * HandleMouse_t)(void *self, unsigned msg, int x, int y, unsigned wparam, unsigned unused);
    char *bd = battle();
    *(int *)(bd + L.bd_current_tile) = cur_x; // DoPick sets it each frame; a key can come in between
    *(int *)(bd + L.bd_current_tile + 4) = cur_y;
    ((HandleMouse_t)eng::fn_handle_mouse)(bd, WM_LBUTTONDOWN, -1, -1, MK_LBUTTON, 0);
    ((HandleMouse_t)eng::fn_handle_mouse)(bd, WM_LBUTTONUP, -1, -1, 0, 0);
    logf("battle: left click on tile %d,%d", cur_x, cur_y);
}

// Carry out order i on the cursor tile the way clicking its icon does: mSelectedTile = the tile,
// DoActionOnCurrentUnit(i), mSelectedTile = none. A greyed-out order is not done: its text says why.
void do_order(int i)
{
    typedef void(__thiscall * DoAction_t)(void *self, int option);
    char *bd = battle();
    if (!order_enabled(i)) {
        speak(order_text(i));
        return;
    }
    logf("battle: order %d (%ls) on tile %d,%d", i, order_text(i).c_str(), cur_x, cur_y);
    *(int *)(bd + L.bd_selected_tile) = cur_x;
    *(int *)(bd + L.bd_selected_tile + 4) = cur_y;
    std::wstring text = order_text(i);
    ((DoAction_t)eng::fn_do_action)(bd, i);
    *(int *)(bd + L.bd_selected_tile) = -1;
    speak(text, false);
    // Alt+direction steps keep the unit selected for the next step (user, 2026-11-07).
    if (from_step) {
        from_step = false;
        return;
    }
    // Done with this unit (user, 2026-10-31): drop the selection, without the "canceled" a dropped
    // selection otherwise gets.
    typedef void(__thiscall * SelectUnit_t)(void *self, int index, int flag);
    ((SelectUnit_t)eng::fn_select_unit)(bd, -1, 0);
    selected = -1;
}

void close_order_menu()
{
    if (order_menu >= 0)
        *(int *)(battle() + L.bd_selected_tile) = -1;
    order_menu = -1;
}

void speak_order_menu()
{
    speak(order_text(menu_orders[order_menu]) + L", " + std::to_wstring(order_menu + 1) + L" of " +
          std::to_wstring(menu_orders.size()));
}

// Enter: with a unit selected and orders on the tile, its only order, or the list of them (the game's
// icons: the tile stays marked in mSelectedTile so the game keeps them while the list is open);
// otherwise a left click (select our unit there, or drop the selection) unless click is false.
void enter(bool click_if_none = true)
{
    from_step = !click_if_none; // only the Alt+direction step calls it without the click
    char *bd = battle();
    *(int *)(bd + L.bd_current_tile) = cur_x; // DoPick sets it each frame; a key can come in between
    *(int *)(bd + L.bd_current_tile + 4) = cur_y;
    aim_mouse();
    menu_orders.clear();
    for (int i = 0, n = at<int>(bd, L.bd_current_unit) >= 0 ? order_count() : 0; i < n; i++)
        if (!is_undo(i) && !order_blank(i)) // Undo is Alt+Enter, so Enter never takes a move back
            menu_orders.push_back(i);
    int n = (int)menu_orders.size();
    if (n == 1) {
        do_order(menu_orders[0]);
    } else if (n > 1) {
        *(int *)(bd + L.bd_selected_tile) = cur_x;
        *(int *)(bd + L.bd_selected_tile + 4) = cur_y;
        order_menu = 0;
        speak_order_menu();
    } else if (click_if_none && (at<int>(bd, L.bd_current_unit) < 0 || order_count() == 0)) {
        click(); // with only Undo offered, Enter does nothing
    }
}

// Alt+Enter: the Undo order on the cursor square, if the game offers one.
void undo()
{
    char *bd = battle();
    *(int *)(bd + L.bd_current_tile) = cur_x;
    *(int *)(bd + L.bd_current_tile + 4) = cur_y;
    aim_mouse();
    from_step = false;
    for (int i = 0, n = at<int>(bd, L.bd_current_unit) >= 0 ? order_count() : 0; i < n; i++)
        if (is_undo(i)) {
            do_order(i);
            return;
        }
}

// Alt+direction: the cursor goes to the square next to the selected unit that way, and once the game
// has worked out that square's orders (two DoPick frames, see the tick) the order is given as Enter
// would, without the left click when there is none.
void unit_step(int dx, int dy)
{
    Map m;
    char *u = unit_by_index(at<int>(battle(), L.bd_current_unit));
    if (!u || !get_map(m))
        return;
    int x = at<int>(u, L.unit_tile) + dx, y = at<int>(u, L.unit_tile + 4) + dy;
    if (x < 0 || y < 0 || x >= m.w || y >= m.h) {
        speak(L"edge");
        return;
    }
    cur_x = x;
    cur_y = y;
    speak_tile(m);
    step_pending = true;
}

bool order_menu_key(WPARAM vk)
{
    int n = (int)menu_orders.size();
    if (order_count() == 0 || n == 0) {
        close_order_menu();
        return false;
    }
    switch (vk) {
    case VK_DOWN: order_menu = std::min(order_menu + 1, n - 1); speak_order_menu(); return true;
    case VK_UP: order_menu = std::max(order_menu - 1, 0); speak_order_menu(); return true;
    case VK_HOME: order_menu = 0; speak_order_menu(); return true;
    case VK_END: order_menu = n - 1; speak_order_menu(); return true;
    case VK_RETURN: {
        int i = menu_orders[order_menu];
        order_menu = -1;
        do_order(i); // clears mSelectedTile
        return true;
    }
    case VK_ESCAPE: close_order_menu(); speak(L"canceled"); return true;
    }
    close_order_menu(); // any other key closes it and does its own thing
    return false;
}

// Escape: drop the selection, if there is one.
bool deselect()
{
    typedef void(__thiscall * SelectUnit_t)(void *self, int index, int flag);
    char *bd = battle();
    if (at<int>(bd, L.bd_current_unit) < 0)
        return false;
    ((SelectUnit_t)eng::fn_select_unit)(bd, -1, 0); // what a left click on an empty tile calls
    return true; // the tick says "canceled"
}

// --- the HUD ---

void *hud(const wchar_t *name)
{
    char *s = screen("BATTLECOMMON");
    return s ? find(at<void *>(s, L.screen_root), name) : nullptr;
}

std::wstring hud_text(const wchar_t *name)
{
    void *o = hud(name);
    std::wstring t = o ? trim(eng::plain(eng::kstring(o, L.text_string))) : L"";
    for (wchar_t &c : t)
        if (c == L'\n')
            c = L',';
    return t;
}

// T: the turn and the score panel (each side's rout percentage).
void read_turn()
{
    // User, 2026-11-07: "Turn 4. Ancient British 0%, Roman 7%". The button text's last line is always the
    // button's own label (BattleCommon.bsf: "Turn n[/m, k remaining]\nEnd Turn"), and the score's bracket
    // is the difference to the other side, shown by colour on screen: both left out.
    std::wstring s = hud_text(L"ENDTURNBUTTONTEXT");
    size_t last = s.rfind(L',');
    if (last != std::wstring::npos)
        s.erase(last);
    for (int side = 0; side < 2; side++) {
        std::wstring score = hud_text(side ? L"BATTLE_SCORESIDE1SCORE" : L"BATTLE_SCORESIDE0SCORE");
        score = trim(score.substr(0, score.find(L'(')));
        s += (side ? L", " : L". ") + faction(side) + L" " + score;
    }
    speak(s);
}

// R: the battle's name, date and briefing (objectives).
void read_briefing()
{
    std::wstring s = hud_text(L"BATTLE_SUBTITLETEXTBATTLENAME");
    for (const wchar_t *name : {L"BATTLE_SUBTITLETEXTBATTLEDATE", L"BATTLE_BRIEFINGINFO"}) {
        std::wstring t = hud_text(name);
        if (!t.empty())
            s += L". " + t;
    }
    speak(s);
}

bool shown(void *obj);
std::wstring first_line(std::wstring s);

// The End Turn prompt (BattleCommon.bsf): pressing End Turn sets the screen script's gEndTurnConfirm, and
// its update swaps the wreath for one with a Confirm button (and the End Turn button again, which takes the
// question back). A click anywhere else also takes it back. The update doesn't run while the window is
// minimized, so the flag decides, not what's drawn.
bool end_turn_asking()
{
    char *bc = screen("BATTLECOMMON");
    int *asked = bc ? (int *)eng::script_var(at<void *>(bc, L.screen_root), "gEndTurnConfirm") : nullptr;
    void *confirm = hud(L"CONFIRMENDTURNBUTTON");
    return confirm && (shown(confirm) || (asked && *asked == 1));
}

// Spoken after N: the turn counter, then the prompt as a one-item list (user, 2026-11-09: what the first N
// shows must be accessible): "Turn 1/24, 23 left. Confirm, 1 of 1". Enter or N confirms, Escape takes
// it back ("canceled"), any other key takes it back and does its own thing.
void read_end_turn()
{
    // "Turn 1/24\n23 left\nEnd Turn": all but the last line (the button's own label).
    std::wstring t = hud_text(L"ENDTURNBUTTONTEXT"), s;
    size_t cut = t.find_last_of(L"\n,");
    if (cut != std::wstring::npos)
        t.erase(cut);
    for (wchar_t c : t)
        s += c == L',' || c == L'\n' ? std::wstring(L", ") : std::wstring(1, c);
    if (end_turn_asking()) {
        end_prompt = true;
        void *confirm = hud(L"CONFIRMENDTURNBUTTON");
        s += L". " + trim(eng::plain(eng::kstring(confirm, L.button_string))) + L", 1 of 1";
    }
    speak(s);
}

void take_back_end_turn()
{
    end_prompt = false;
    if (end_turn_asking())
        if (void *again = hud(L"ENDTURNBUTTON2")) // BattleCommon.bsf: gEndTurnConfirm = 0
            eng::trigger(again);
}

// N: the End Turn button; while the game asks for confirmation, its Confirm button.
void end_turn()
{
    end_prompt = false;
    // Battlefield review (EndReview.txt): N presses its End Review button, back to the results.
    char *review = screen("ENDREVIEW");
    if (review && !at<bool>(review, L.screen_hidden)) {
        if (void *ok = find(at<void *>(review, L.screen_root), L"ENDREVIEWOK"))
            eng::trigger(ok);
        return;
    }
    if (end_turn_asking()) {
        eng::trigger(hud(L"CONFIRMENDTURNBUTTON"));
        last_user_key = 0; // the turn is the game's now: its selections aren't the user's
        return;
    }
    void *button = hud(L"ENDTURNBUTTON");
    if (!shown(button))
        return;
    eng::trigger(button);
    end_turn_speech = 6; // about 0.1 s: the screen script updates the buttons first
}

// R (user, 2026-11-09): retreat, as the briefing frame's exit button (BattleRetreatNew, tooltip
// IDS_TT_EXIT_BATTLE) does. Retreating loses the battle, so the first R only asks, in the button's words:
// Enter or R presses it, Escape cancels, any other key cancels and acts.
void ask_retreat()
{
    void *b = hud(L"BATTLERETREATNEW");
    if (!b)
        return;
    std::wstring label = trim(eng::plain(eng::kstring(b, L.obj_tooltip)));
    if (label.empty())
        label = eng::game_string("IDS_BTTOOL_RETREAT");
    retreat_prompt = true;
    speak(label + L", 1 of 1");
}

bool end_prompt_key(WPARAM vk)
{
    switch (vk) {
    case VK_RETURN: case 'N': end_turn(); return true;
    case VK_ESCAPE: take_back_end_turn(); speak(L"canceled"); return true;
    case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: read_end_turn(); return true;
    }
    take_back_end_turn(); // any other key: the question goes, the key does its own thing
    return false;
}

bool shown(void *obj) { return obj && at<bool>(obj, L.obj_visible); }

std::wstring first_line(std::wstring s)
{
    size_t nl = s.find(L',');
    return trim(nl == std::wstring::npos ? s : s.substr(0, nl));
}

// The turn changing hands: "Enemy Turn" (EnemyTurnImage, BattleCommon.bsf BattleCommon_SetUI) and back
// ("Turn 2", the End Turn button's text).
void watch_turn()
{
    bool now = shown(hud(L"ENEMYTURNIMAGE"));
    if (now == enemy_turn)
        return;
    enemy_turn = now;
    note(TURNS, now ? hud_text(L"ENEMYTURNTEXT") : first_line(hud_text(L"ENDTURNBUTTONTEXT")));
}

// Deployment instructions (DeployMessage screen, DeployMessageText) whenever they change.
void watch_deploy()
{
    char *s = screen("DEPLOYMESSAGE");
    void *text = s && !at<bool>(s, L.screen_hidden) ? find(at<void *>(s, L.screen_root), L"DEPLOYMESSAGETEXT") : nullptr;
    std::wstring t = text ? joined(text) : L"";
    if (t != deploy_said) {
        deploy_said = t;
        note(DEPLOY, t, false); // the menu module reads the screen itself (2026-11-09: it was said twice)
    }
}

// Units: an enemy unit that moved is spoken once it has stood still for half a second ("moved to x, y"),
// one coming into view as "spotted"; any unit's morale state changing (IDS_MORALESTATE: Steady,
// Disrupted, Fragmented, Routed...). The first look at a battle only records.
void watch_units(const Map &m)
{
    static int morale_attr = -2, men_attr = -2;
    if (morale_attr == -2) {
        morale_attr = attrib_index("MoraleState");
        men_attr = attrib_index("DisplayTotalMen"); // follows what the screen shows
        if (men_attr < 0)
            men_attr = attrib_index("TotalMen");
    }
    int me = my_side();
    for (int side = 0; side < 2; side++) {
        char *s = battle() + L.bd_sides + side * L.side_size;
        unsigned n = std::min(at<unsigned>(s, L.side_unit_count), 256u);
        for (unsigned i = 0; i < n; i++) {
            char *u = s + L.side_units + i * L.unit_size;
            bool see = visible(m, u);
            int x = at<int>(u, L.unit_tile), y = at<int>(u, L.unit_tile + 4);
            int morale = morale_attr >= 0 ? ((const int *)(u + L.unit_attrib))[morale_attr] : 0;
            int men = men_attr >= 0 ? ((const int *)(u + L.unit_attrib))[men_attr] : 0;
            auto hit = tracks.find(u);
            if (hit == tracks.end() || !tracks_ready) {
                tracks[u] = {x, y, x, y, 0, see, morale, men, 0, 0};
                continue;
            }
            Track &t = hit->second;
            std::wstring who = unit_name(u) + (side != me ? L", " + faction(side) : L"");
            // Men lost (user, 2026-11-08: "slingers attacked this and it lost 8 men"): added up while
            // the casualties play out, spoken half a second after the last one.
            if (men < t.men) {
                t.lost += t.men - men;
                t.quiet = 0;
            } else if (t.lost > 0 && ++t.quiet == 30) {
                if (see || side == me)
                    note(COMBAT, who + L", lost " + std::to_wstring(t.lost) + L" men, " + std::to_wstring(men) + L" left");
                t.lost = 0;
            }
            t.men = men;
            if (see && morale != t.morale) {
                std::string key = "IDS_MORALESTATE" + std::to_string(morale);
                std::wstring state = eng::game_string(key.c_str());
                if (!state.empty())
                    note(MORALE, who + L", " + state);
            }
            t.morale = morale;
            if (side != me) {
                if (see && !t.seen) {
                    note(MOVES, who + L", spotted, " + std::to_wstring(x) + L", " + std::to_wstring(y) + L", " + offset(x, y));
                    t.said_x = x;
                    t.said_y = y;
                }
                t.still = x == t.x && y == t.y ? t.still + 1 : 0;
                if (see && t.still == 30 && (x != t.said_x || y != t.said_y)) {
                    note(MOVES, who + L", moved to " + std::to_wstring(x) + L", " + std::to_wstring(y) + L", " + offset(x, y));
                    t.said_x = x;
                    t.said_y = y;
                }
            }
            t.x = x;
            t.y = y;
            t.seen = see;
        }
    }
    tracks_ready = true;
}

// --- scanner ---

struct Group {
    std::wstring name;
    std::vector<Unit> units;
};

std::vector<Group> groups(const Map &m, int side)
{
    std::vector<Group> out;
    char *s = battle() + L.bd_sides + side * L.side_size;
    unsigned n = at<unsigned>(s, L.side_unit_count);
    for (unsigned i = 0; i < n && i < 256; i++) {
        char *u = s + L.side_units + i * L.unit_size;
        if (!visible(m, u) || parked(m, u)) // units waiting off the battlefield: not scanned (user, 2026-11-10)
            continue;
        Unit unit{u, side, at<int>(u, L.unit_tile), at<int>(u, L.unit_tile + 4), unit_name(u)};
        size_t g = 0;
        while (g < out.size() && out[g].name != unit.name)
            g++;
        if (g == out.size())
            out.push_back({unit.name, {}});
        out[g].units.push_back(unit);
    }
    // Nearest first (user, 2026-11-07), from the cursor, in moves (8 directions: the larger of the two
    // differences): units within a type, and types by their nearest unit.
    auto dist = [](const Unit &u) { return std::max(abs(u.x - cur_x), abs(u.y - cur_y)); };
    for (Group &g : out)
        std::stable_sort(g.units.begin(), g.units.end(), [&](const Unit &a, const Unit &b) { return dist(a) < dist(b); });
    std::stable_sort(out.begin(), out.end(), [&](const Group &a, const Group &b) { return dist(a.units[0]) < dist(b.units[0]); });
    return out;
}

// Scanner categories: our units, the enemy's (grouped by unit name), and generals (user, 2026-11-07):
// every unit with a general, both sides, each one an item of its own (no groups), nearest first. A
// general's unit also stays with its kind in its side's category.
const int SCAN_CATS = 3, GENERALS = 2;

std::wstring cat_name(int cat)
{
    if (cat == GENERALS)
        return L"Generals";
    // Which side is which, not only the faction's name (user, 2026-11-09): "You: Ancient British",
    // "Enemy: Roman".
    return (cat ? L"Enemy: " : L"You: ") + faction(cat ? 1 - my_side() : my_side());
}

std::vector<Group> groups_for(const Map &m, int cat)
{
    if (cat != GENERALS)
        return groups(m, cat ? 1 - my_side() : my_side());
    std::vector<Group> out;
    for (int side = 0; side < 2; side++)
        for (Group &g : groups(m, side))
            for (Unit &u : g.units)
                if (!general(u.obj).empty())
                    out.push_back({name_and_general(u.obj, true) +(side != my_side() ? L", " + faction(side) : L""), {u}});
    auto dist = [](const Unit &u) { return std::max(abs(u.x - cur_x), abs(u.y - cur_y)); };
    std::stable_sort(out.begin(), out.end(), [&](const Group &a, const Group &b) { return dist(a.units[0]) < dist(b.units[0]); });
    return out;
}

void speak_item(const std::vector<Group> &gs, size_t g, size_t u, bool with_category)
{
    const Unit &unit = gs[g].units[u];
    scan_name = gs[g].name;
    scan_unit = unit.obj;
    std::wstring s = with_category ? cat_name(scan_cat) + L", " : L"";
    s += gs[g].name;
    if (scan_cat != GENERALS)
        s += L" " + std::to_wstring(u + 1) + L" of " + std::to_wstring(gs[g].units.size());
    if (is_selected(unit.obj))
        s += L", selected";
    s += L", " + offset(unit.x, unit.y) + L", " + std::to_wstring(g + 1) + L" of " + std::to_wstring(gs.size());
    speak(s);
}

enum Step { CATEGORY, ITEM, SAME };

void scan(int dir, Step step)
{
    Map m;
    if (!get_map(m))
        return;
    if (step == CATEGORY)
        scan_cat = (scan_cat + (dir > 0 ? 1 : SCAN_CATS - 1)) % SCAN_CATS;
    if (step == CATEGORY) {
        std::vector<Group> gs = groups_for(m, scan_cat);
        if (gs.empty()) {
            scan_unit = nullptr;
            speak(cat_name(scan_cat) + L", empty");
        } else {
            speak_item(gs, 0, 0, true);
        }
        return;
    }
    std::vector<Group> gs = groups_for(m, scan_cat);
    if (gs.empty()) {
        speak(cat_name(scan_cat) + L", empty");
        return;
    }
    // Where we are: the same unit, else its type, else the start.
    size_t g = gs.size(), u = 0;
    for (size_t i = 0; i < gs.size() && g == gs.size(); i++)
        for (size_t j = 0; j < gs[i].units.size(); j++)
            if (gs[i].units[j].obj == scan_unit)
                g = i, u = j;
    for (size_t i = 0; i < gs.size() && g == gs.size(); i++)
        if (gs[i].name == scan_name) {
            g = i;
            for (size_t j = 0; j < gs[i].units.size(); j++)
                if (gs[i].units[j].obj == scan_unit)
                    u = j;
        }
    if (g == gs.size()) {
        speak_item(gs, 0, 0, false);
        return;
    }
    if (step == ITEM) {
        g = dir > 0 ? (g + 1 < gs.size() ? g + 1 : g) : (g > 0 ? g - 1 : 0);
        u = 0;
    } else {
        size_t n = gs[g].units.size();
        u = dir > 0 ? (u + 1 < n ? u + 1 : u) : (u > 0 ? u - 1 : 0);
    }
    speak_item(gs, g, u, false);
}

// The scanner's unit if it is still there.
Unit *scanned(std::vector<Group> &gs)
{
    for (Group &g : gs)
        for (Unit &u : g.units)
            if (u.obj == scan_unit)
                return &u;
    return nullptr;
}

void scan_go(bool move_there)
{
    Map m;
    if (!get_map(m))
        return;
    std::vector<Group> gs = groups_for(m, scan_cat);
    Unit *u = scanned(gs);
    if (!u) {
        speak(L"none");
        return;
    }
    if (move_there) {
        cur_x = u->x;
        cur_y = u->y;
        speak_tile(m);
    } else {
        speak(offset(u->x, u->y));
    }
}

// --- the DoPick hook ---

typedef void(__thiscall *DoPick_t)(void *self);

void __fastcall do_pick(void *self, void *)
{
    ((DoPick_t)(void *)trampoline)(self);
    picks++;
    if (owns && cur_x >= 0) {
        *(int *)((char *)self + L.bd_current_tile) = cur_x; // CMapPos {x, y}
        *(int *)((char *)self + L.bd_current_tile + 4) = cur_y;
    }
}

// A popup the player has to answer: visible buttons (the unit information popup, messages). The
// tutorial's non-modal box doesn't count: it stays up while the player plays.
bool has_button(void *obj)
{
    for (int budget = 3000; obj && budget > 0; obj = at<void *>(obj, L.obj_sibling), budget--) {
        if (!at<bool>(obj, L.obj_visible))
            continue;
        RECT b = at<RECT>(obj, L.obj_box);
        if ((eng::kinds(obj) & eng::BUTTON) && b.right > b.left && b.bottom > b.top && b.right > 0 && b.bottom > 0)
            return true;
        if (has_button(at<void *>(obj, L.obj_children)))
            return true;
    }
    return false;
}

bool compute_owns()
{
    if (!battle() || !*(char **)eng::battle_map_ptr)
        return false;
    char *ui = *(char **)eng::ui_manager_ptr;
    for (int i = 23; ui && i >= 0; i--) {
        char *s = *(char **)(ui + L.ui_layers + i * L.layer_size);
        if (!s || at<bool>(s, L.screen_hidden))
            continue;
        if (!lstrcmpiA(s + L.screen_name, "BATTLECOMMON"))
            return true;
        if (!lstrcmpiA(s + L.screen_name, "ENDREVIEW"))
            continue; // the battlefield review: the map is the user's to look over (2026-11-09)
        if (at<unsigned>(s, L.screen_flags) & 1)
            return false;
        if (battle_popup(s))
            return false;
    }
    return false;
}

} // namespace

void battle_start()
{
    BYTE *target = (BYTE *)eng::fn_do_pick;
    if (!target || memcmp(target, PROLOGUE, sizeof PROLOGUE)) {
        logf("battle: DoPick doesn't start as expected; not hooked");
        return;
    }
    trampoline = (BYTE *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline)
        return;
    // The trampoline runs DoPick's first 9 bytes, then jumps to the rest.
    memcpy(trampoline, target, 9);
    trampoline[9] = 0xE9;
    *(int32_t *)(trampoline + 10) = (int32_t)(target + 9 - (trampoline + 14));
    BYTE patch[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    *(int32_t *)(patch + 1) = (int32_t)((BYTE *)&do_pick - (target + 5));
    DWORD old;
    VirtualProtect(target, 9, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, patch, 9);
    VirtualProtect(target, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 9);
    hooked = true;
    logf("battle: DoPick hooked");
}

void battle_stop()
{
    if (owns && battle())
        close_order_menu();
    if (hooked) {
        BYTE *target = (BYTE *)eng::fn_do_pick;
        DWORD old;
        VirtualProtect(target, 9, PAGE_EXECUTE_READWRITE, &old);
        memcpy(target, PROLOGUE, 9);
        VirtualProtect(target, 9, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 9);
    }
    hooked = false;
    owns = false;
}

void battle_free()
{
    if (trampoline)
        VirtualFree(trampoline, 0, MEM_RELEASE);
    trampoline = nullptr;
}

void battle_tick()
{
    eng::stage = "battle.tick";
    owns = compute_owns();
    if (!owns)
        return;
    Map m;
    if (!get_map(m))
        return;
    char *map = *(char **)eng::battle_map_ptr;
    if (map != seen_map || cur_x >= m.w || cur_y >= m.h || cur_x < 0) {
        seen_map = map;
        tracks.clear();
        tracks_ready = false;
        history.clear();
        buffer_index = -1;
        place_cursor(m);
        scan_unit = nullptr;
        scan_name.clear();
        scan_cat = 0;
        selected = at<int>(battle(), L.bd_current_unit);
    }
    if (step_pending && picks - picks_at_move >= 2) {
        step_pending = false;
        orders_said = orders(); // spoken by the order (list) itself
        enter(false);
    }
    // The game's texts for the newly hovered tile.
    if (watch_until) {
        if (GetTickCount() > watch_until) {
            watch_until = 0;
        } else {
            std::vector<std::pair<void *, std::wstring>> now;
            all_texts(now);
            for (auto &[obj, text] : now) {
                bool old = false;
                for (auto &b : texts_before)
                    if (b.first == obj && b.second == text) {
                        old = true;
                        break;
                    }
                if (!old)
                    speak(text, false);
            }
            texts_before.swap(now);
            if (picks - picks_at_move >= 2 && at<int>(battle(), L.bd_current_unit) >= 0) {
                std::wstring o = orders();
                if (o != orders_said) {
                    if (!o.empty())
                        speak(o, false);
                    orders_said = o;
                }
                // Deployment (user, 2026-11-09): a square outside the selected unit's area offers no Deploy
                // / Swap order and the game draws no highlight there; say so in the game's words, once.
                if (!deploy_checked && at<int>(battle(), L.bd_deploy_mode) != 0) {
                    deploy_checked = true;
                    static std::wstring deploy = eng::game_string("IDS_TT_DEPLOY"),
                                        swap = eng::game_string("IDS_TT_SWAP_UNITS"),
                                        cannot = eng::game_string("IDS_TT_CANNOTUNLOAD");
                    bool can = (!deploy.empty() && o.find(deploy) != std::wstring::npos) ||
                               (!swap.empty() && o.find(swap) != std::wstring::npos);
                    char *here = unit_at(m, cur_x, cur_y);
                    if (!can && !is_selected(here))
                        speak(cannot.empty() ? L"Cannot deploy here" : cannot, false);
                }
            }
        }
    }
    aim_mouse();
    // The combat log: what each attack did (the game adds its lines even while the log is hidden).
    // Diagnostics (2026-11-07: a charge was carried out and no combat line was spoken).
    static void *log_seen = (void *)1;
    static int count_seen = -1;
    void *log_obj = hud(L"BATTLECOMBATLOG");
    if (log_obj != log_seen) {
        log_seen = log_obj;
        logf("combat log: %s", log_obj ? "found" : "not found");
    }
    if (log_obj && eng::list_count(log_obj) != count_seen) {
        count_seen = eng::list_count(log_obj);
        logf("combat log: %d lines", count_seen);
    }
    if (void *log = log_obj) {
        int n = eng::list_count(log);
        if (log != log_list || n < log_said) {
            log_list = log;
            log_said = n; // a new battle or a cleared log: only lines from now on
        }
        // The log's lines are its display rows (words cut mid-sentence): this tick's new rows become one
        // buffer entry (the full report, for the buffers) and only the result is spoken; men lost and
        // morale changes are spoken by watch_units (user, 2026-11-08: too long to follow).
        std::wstring block;
        for (; log_said < n; log_said++) {
            std::wstring line = trim(eng::plain(eng::list_item(log, log_said)));
            if (line.empty())
                continue;
            if (!block.empty())
                block += L' ';
            block += line;
        }
        if (!block.empty()) {
            note(COMBAT, block, false);
            size_t r = block.rfind(L"Result: ");
            if (r != std::wstring::npos) {
                size_t end = block.find(L" Enemy Casualties", r);
                size_t end2 = block.find(L" Our Casualties", r);
                end = std::min(end, end2);
                speak(trim(block.substr(r, end == std::wstring::npos ? 60 : end - r)), false);
            }
        }
    }
    watch_turn();
    watch_deploy();
    {
        // The battlefield review starting: its text (ids_End_Review) once.
        static bool was_review = false;
        char *review = screen("ENDREVIEW");
        bool now = review && !at<bool>(review, L.screen_hidden);
        if (now && !was_review)
            speak(trim(joined(find(at<void *>(review, L.screen_root), L"ENDREVIEWTEXT"))), false);
        was_review = now;
    }
    watch_units(m);
    if (end_turn_speech && !--end_turn_speech)
        read_end_turn();
    // A selection made another way is spoken once it has held for 10 ticks (~0.16 s): when deployment
    // ends, the game runs through every unit's selection within a second (2026-11-09: 20 announcements).
    int sel = at<int>(battle(), L.bd_current_unit);
    static int pending = -2, held = 0;
    if (sel != pending) {
        pending = sel;
        held = 0;
    } else if (held < 10) {
        held++;
    }
    // Only selections the user caused (a key in the last 1.5 s: Tab, B, the unit list) are spoken and move
    // the cursor; the game also moves the selection by itself while it plays a turn out (2026-11-09).
    if (sel != selected && held >= 10 && GetTickCount() - last_user_key > 1500)
        selected = sel;
    if (sel != selected && held >= 10) {
        selected = sel;
        if (char *u = unit_by_index(sel)) {
            // Selected another way (the unit list, the game's Tab / B): the cursor goes to it, and how
            // far it went is said (user, 2026-11-07): "Warband, selected, 3 north, 2 east".
            int x = at<int>(u, L.unit_tile), y = at<int>(u, L.unit_tile + 4);
            std::wstring moved = offset(x, y);
            speak(name_and_general(u) + L", selected" + (moved == L"here" ? L"" : L", " + moved), false);
            cur_x = x;
            cur_y = y;
        }
        else
            speak(L"canceled", false);
    }
}

bool battle_owns() { return owns; }

namespace {
Stats unit_stats(char *u)
{
    Stats st;
    static int men = -1, shown = -1, size = -1, ap = -1, morale = -1;
    if (men < 0) {
        men = attrib_index("TotalMen");
        shown = attrib_index("DisplayTotalMen");
        size = attrib_index("UnitSize");
        ap = attrib_index("AP");
        morale = attrib_index("MoraleState");
    }
    const int *now = (const int *)(u + L.unit_attrib);
    char *side_data = *(char **)eng::side_data_ptr;
    unsigned type = at<unsigned>(u, L.unit_type);
    char *templ = type < 1280 ? ((char **)(side_data + L.sd_templates))[type] : nullptr;
    if (templ && men >= 0 && size >= 0) {
        const int *base = (const int *)(templ + L.template_attrib);
        if (base[size] > 0)
            st.strength = std::to_wstring(now[shown >= 0 ? shown : men]) + L"/" +
                          std::to_wstring(base[men] * now[size] / base[size]);
    }
    if (ap >= 0)
        st.ap = eng::game_string("IDS_TT_AP") + L": " + std::to_wstring(now[ap]);
    if (morale >= 0) {
        std::string key = "IDS_MORALESTATE" + std::to_string(now[morale]);
        st.morale = eng::game_string(key.c_str());
    }
    return st;
}
} // namespace

std::wstring battle_list_entry(void *list, int index)
{
    // UnitList.bsf UI_ITEM_RENDER(..., me): name, PrintStrengthString, "AP: n", PrintMoraleDisorderString
    // (here the morale state alone: the disorder part is worked out by the script from terrain and combat).
    if (!battle() || eng::name(list) != L"UNITLISTLIST")
        return {};
    char *u = unit_by_index((int)eng::list_item_data(list, index));
    if (!u)
        return {};
    Stats st = unit_stats(u);
    std::wstring s = name_and_general(u);
    for (const std::wstring *p : {&st.strength, &st.ap, &st.morale})
        if (!p->empty())
            s += L", " + *p;
    return s;
}

// Force selection rows (ForceSelectionTools.bsf ForceSelect_RenderListItem draws them; only the name is
// stored in the entry): the script's gForceSelect[row] {typeIndex, fixed, unfixed, selected, sequestered}
// and GetUnitTypeCost(type) = the squad template's mCost. Labels are the column headers ("COST", "QUANTITY").
// Verified live 2026-11-09: fixed units' costs add up to the points already spent.
std::wstring battle_force_details(void *list, int index, bool quantity_only)
{
    if (eng::name(list) != L"FORCEOVLY_LIST")
        return {};
    char *rows = (char *)eng::script_var(list, "gForceSelect");
    int size = 0;
    int o_type = eng::script_member(list, "ForceSelect", "typeIndex", &size);
    int o_fixed = eng::script_member(list, "ForceSelect", "fixed", nullptr);
    int o_unfixed = eng::script_member(list, "ForceSelect", "unfixed", nullptr);
    int o_selected = eng::script_member(list, "ForceSelect", "selected", nullptr);
    int o_seq = eng::script_member(list, "ForceSelect", "sequestered", nullptr);
    if (!rows || size <= 0 || index < 0 || index >= 64 || o_type < 0 || o_fixed < 0 || o_unfixed < 0 || o_selected < 0 || o_seq < 0)
        return {};
    char *r = rows + index * size;
    int type = at<int>(r, o_type), fixed = at<int>(r, o_fixed), unfixed = at<int>(r, o_unfixed);
    int selected = at<int>(r, o_selected), seq = at<int>(r, o_seq);
    // The units column exactly as drawn: "fixed [+ ]bought/available".
    std::wstring units;
    if (fixed > 0 || seq > 0)
        units = std::to_wstring(fixed + seq) + (unfixed - seq > 0 ? L" + " : L"");
    if (unfixed - seq > 0)
        units += std::to_wstring(selected - seq) + L"/" + std::to_wstring(unfixed - seq);
    auto header = [&](const wchar_t *name) {
        char *ov = screen("FORCEOVERLAY");
        void *root = ov ? at<void *>(ov, L.screen_root) : nullptr;
        std::wstring t = trim(joined(root ? find(root, name) : nullptr));
        for (size_t i = 1; i < t.size(); i++) // "COST" -> "Cost"
            t[i] = towlower(t[i]);
        return t;
    };
    std::wstring s = header(L"FORCEOVLY_UNITSHEADER") + L" " + units;
    if (quantity_only)
        return s;
    char *side_data = *(char **)eng::side_data_ptr;
    char *templ = side_data && type >= 0 && type < 1280 ? ((char **)(side_data + L.sd_templates))[type] : nullptr;
    if (templ)
        s = header(L"FORCEOVLY_COSTHEADER") + L" " + std::to_wstring(at<unsigned>(templ, L.template_cost)) + L", " + s;
    return s;
}

bool battle_list_pick(void *list, int index)
{
    if (!battle() || eng::name(list) != L"UNITLISTLIST")
        return false;
    int unit = (int)eng::list_item_data(list, index); // side * 1000 + i, CBattleDisplay::mCurrentUnit's form
    if (!unit_by_index(unit))
        return false;
    typedef void(__thiscall * SelectUnit_t)(void *self, int index, int flag);
    last_user_key = GetTickCount();
    ((SelectUnit_t)eng::fn_select_unit)(battle(), unit, 0); // the tick then moves the cursor to it
    logf("battle: unit list picked unit %d", unit);
    return true;
}

bool battle_popup(char *screen)
{
    const char *name = screen + L.screen_name;
    return battle() && !battle_hud(name) && !strstr(name, "NONMODAL") && has_button(at<void *>(screen, L.screen_root));
}

bool battle_hud(const char *name)
{
    // ENDREVIEW: the battlefield review's End Review wreath (one OK button): N presses it.
    static const char *const hud[] = {"", "BATTLECOMMON", "BATTLEADVANCEDPANEL", "BATTLEADVANCEDPANEL_LEFT", "ENDREVIEW"};
    for (const char *h : hud)
        if (!lstrcmpiA(name, h))
            return true;
    return false;
}

bool battle_blocks(WPARAM vk)
{
    // The game's F1 list: J minimap, G grid, M top-down view, F/R and the arrows zoom, rotate and tilt the
    // camera, 1/2/3 draw line of fire / line of sight / command range (spoken instead). Tab, B, L, K,
    // Space, Escape, F1 and F2 stay the game's.
    if (!owns || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
        return false;
    switch (vk) {
    case 'J': case 'G': case 'M': case 'F': case 'R': case '1': case '2': case '3':
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        return true;
    }
    return false;
}

// F1 in battle (user, 2026-11-07): the mod's keys instead of the game's list. Up/Down/Home/End read,
// Escape or F1 closes, any other key closes it and does its own thing.
const wchar_t *const key_help[] = {
    L"Q, W, E, A, D, Z, X, C: move the cursor",
    L"Ctrl plus a direction: move the cursor with the game's detailed tooltips",
    L"Alt plus a direction: order the selected unit to the square next to it that way; it stays selected",
    L"Enter: select the unit, or give the order on the cursor square",
    L"Alt+Enter: undo the move",
    L"Ctrl+Enter: unit information",
    L"Escape: deselect",
    L"S: the unit's AP, men, morale and details",
    L"T: turn and scores",
    L"O: battle name, date and briefing",
    L"R: retreat from the battle; Enter or R again to confirm, Escape to cancel",
    L"N: end turn; Enter or N again to confirm, Escape to cancel. In the battlefield review: end the review",
    L"Page Up, Page Down: scanner, previous or next unit type, nearest first",
    L"Alt+Page Up, Alt+Page Down: previous or next unit of the same type",
    L"Ctrl+Page Up, Ctrl+Page Down: scanner category: our units, enemy units, generals",
    L"Home: go to the scanned unit; End: where it is from the cursor",
    L"Left and right brackets: message category; comma, period: older, newer message",
    L"Shift with brackets: first, last category; Shift with comma, period: oldest, newest message",
    L"L: unit list, Enter selects the unit",
    L"K: casualties",
    L"Tab: next unit that hasn't moved; B: next unit that can shoot",
    L"Space: close a message or proceed",
    L"F2: screenshot",
};
int help_at = -1;

void speak_help()
{
    const int n = (int)(sizeof key_help / sizeof key_help[0]);
    speak(std::wstring(key_help[help_at]) + L", " + std::to_wstring(help_at + 1) + L" of " + std::to_wstring(n));
}

bool help_key(WPARAM vk, bool act)
{
    const int n = (int)(sizeof key_help / sizeof key_help[0]);
    int to;
    switch (vk) {
    case VK_DOWN: to = std::min(help_at + 1, n - 1); break;
    case VK_UP: to = std::max(help_at - 1, 0); break;
    case VK_HOME: to = 0; break;
    case VK_END: to = n - 1; break;
    case VK_ESCAPE:
    case VK_F1:
        if (act) {
            help_at = -1;
            speak(L"closed");
        }
        return true;
    default:
        if (act)
            help_at = -1; // and the key does its own thing
        return false;
    }
    if (act) {
        help_at = to;
        speak_help();
    }
    return true;
}

bool battle_key(WPARAM vk, bool act)
{
    eng::stage = "battle.key";
    if (!owns || cur_x < 0) {
        help_at = -1;
        return false;
    }
    if (act)
        last_user_key = GetTickCount(); // also Tab / B, which go on to the game
    if (help_at >= 0 && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && help_key(vk, act))
        return true;
    if (vk == VK_F1 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
        if (act) {
            help_at = 0;
            speak_help();
        }
        return true;
    }
    static const struct {
        WPARAM vk;
        int dx, dy;
    } steps[] = {{'Q', -1, -1}, {'W', 0, -1}, {'E', 1, -1}, {'A', -1, 0},
                 {'D', 1, 0},   {'Z', -1, 1}, {'X', 0, 1},  {'C', 1, 1}};
    bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (order_menu >= 0 && !ctrl && !alt && !shift && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
        if (!act)
            return vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END || vk == VK_RETURN || vk == VK_ESCAPE;
        if (order_menu_key(vk))
            return true;
    }
    if (retreat_prompt && !ctrl && !alt && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
        if (!act)
            return vk == VK_RETURN || vk == 'R' || vk == VK_ESCAPE;
        retreat_prompt = false;
        if (vk == VK_RETURN || vk == 'R') {
            if (void *b = hud(L"BATTLERETREATNEW"))
                eng::trigger(b); // its script triggers the engine's DeployRetreat
            return true;
        }
        if (vk == VK_ESCAPE) {
            speak(L"canceled");
            return true;
        }
    }
    if (end_prompt && !ctrl && !alt && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
        if (!act)
            return vk == VK_RETURN || vk == 'N' || vk == VK_ESCAPE || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END;
        if (end_prompt_key(vk))
            return true;
    }
    if (vk == VK_PRIOR || vk == VK_NEXT) {
        if (shift)
            return false;
        if (act)
            scan(vk == VK_NEXT ? 1 : -1, ctrl ? CATEGORY : alt ? SAME : ITEM);
        return true;
    }
    if (act)
        step_pending = false;
    if (alt && !ctrl && !shift && vk == VK_RETURN) {
        if (act)
            undo();
        return true;
    }
    if (alt && !ctrl && !shift) {
        for (auto &s : steps)
            if (vk == s.vk)
            {
                if (act)
                    unit_step(s.dx, s.dy);
                return true; // also with no unit selected: Alt+letter would open nothing useful
            }
    }
    if (ctrl && !alt && !shift) {
        // Ctrl held over a target is the game's "detailed tooltips": the orders spoken after the step
        // are made while Ctrl is still down.
        for (auto &s : steps)
            if (vk == s.vk) {
                if (act)
                    move(s.dx, s.dy);
                return true;
            }
    }
    if (vk == VK_RETURN && ctrl && !alt && !shift) {
        if (act)
            click(); // the game's Ctrl+click on a unit: its information popup
        return true;
    }
    if (!ctrl && !alt && (vk == VK_OEM_4 || vk == VK_OEM_6 || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD)) {
        if (act)
            buffer_step(vk, shift);
        return true;
    }
    if (ctrl || alt || shift)
        return false;
    for (auto &s : steps)
        if (vk == s.vk) {
            if (act)
                move(s.dx, s.dy);
            return true;
        }
    switch (vk) {
    case 'S': if (act) read_unit(); return true;
    case 'T': if (act) read_turn(); return true;
    case 'O': if (act) read_briefing(); return true; // was R until 2026-11-09 (user)
    case 'R': if (act) ask_retreat(); return true;
    case 'N': if (act) end_turn(); return true;
    case VK_RETURN: if (act) enter(); return true;
    case VK_HOME: if (act) scan_go(true); return true;
    case VK_END: if (act) scan_go(false); return true;
    case VK_ESCAPE: return at<int>(battle(), L.bd_current_unit) >= 0 && (!act || deselect());
    }
    return false;
}
