// The game's menus.
//
// The engine keeps the screens that are up in CUIManager::mLayers (one per
// layer); each screen is a tree of UI objects (children/sibling links). From
// the top layer down to the first screen that covers what's below, we take the
// visible, on-screen buttons, texts and progress bars, and rebuild them into
// the controls a sighted player sees:
//   - columns (split at clear horizontal gaps between controls), each cut into
//     rows (elements whose vertical centres are within 20 px);
//   - a text lying on a button is that button's caption;
//   - a row with a progress bar is a slider (Left/Right press its -/+ buttons);
//   - a row of buttons one of which is pressed in (mIndented) is one setting
//     with choices: the game draws "On" and "Off" as two buttons (Enter cycles,
//     Left/Right step);
//   - other buttons are buttons; a row's text names its only button, or else
//     joins the nearest one;
//   - a text-only row just under a control row is that control's value
//     ("Tribune" under Difficulty); any other is a heading, spoken as you enter
//     its part of the screen (a lone title only when the screen opens).
// Enter presses through the engine's own CUIButton::Trigger, as a hotkey would.
//
// Everything runs on the game's main thread: a WH_GETMESSAGE hook sees keys as
// the game takes them from its queue (a key we handle becomes WM_NULL), and a
// 16 ms thread timer, caught by the same hook, rebuilds the list (a walk over a
// few hundred objects, microseconds) and speaks what changed: a new screen, or
// the current control's value after a press.
//
// Keys: Up/Down move, Tab/Shift+Tab move and wrap, Home/End first/last,
// typing jumps to the next control whose name starts with what was typed
// (the same letter again cycles), Enter/Space press, Left/Right change.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <vector>

#include "battle.h"
#include "engine.h"
#include "menus.h"
#include "speech.h"

using eng::at;
using eng::L;
using eng::plain;

namespace {

// LINE: a line of a long text, read only; EDIT: a name box; TABLE: a grid of texts (Final Losses)
// ENTRY: one entry of a list shown as its own menu item (campaigns, scenarios): choices[0] = the list
enum Type { BUTTON, CHOICE, SLIDER, LIST, LINE, EDIT, TABLE, ENTRY };

struct Item {
    Type type;
    void *obj;                  // identity: the button, the first choice, the bar
    std::vector<void *> choices; // CHOICE: left to right; SLIDER: {down, up}; LIST: {the list box}
    std::wstring name;          // what type-search matches
    std::wstring speech;        // name and value, without heading or position
    std::wstring heading;
    std::wstring text;          // EDIT: what is typed in it
    std::vector<std::wstring> rows, cols;      // TABLE: row and column headers
    std::vector<std::vector<std::wstring>> cells; // TABLE: [row][column], "" = blank
    int index = -1;                               // ENTRY: the entry's index in its list
};
int table_row, table_col; // the cell the user is on in the table item

struct El {
    void *obj;
    unsigned kinds;
    RECT box;
    std::wstring text; // button: text or caption; text: its string
    std::wstring tip;
    int column = -1;   // -1: spans columns
    bool used = false;
    int layer = 0;     // the screen's layer: a higher one is on top
};

std::vector<Item> items;
std::wstring title; // the screen's topmost heading
void *current; // identity of the control the user is on
bool ready;
HHOOK hook;
UINT_PTR timer;
DWORD game_thread;
HANDLE stopped;
std::atomic<int> busy;
const UINT MSG_START = WM_APP + 0x4F2, MSG_STOP = WM_APP + 0x4F3;
std::wstring typed; // type-search buffer
DWORD typed_at;
// Texts on screen, to read what changes after the user picks something (a
// scenario's description when it's selected in a list, a tab's contents).
std::vector<std::pair<void *, std::wstring>> texts_now, texts_before;
DWORD watch_until;
DWORD settle_at; // a new screen is spoken once its scripts have filled its texts
DWORD settle_deadline; // ...or by then at the latest
std::wstring settle_sig; // the screen's texts when last looked at while settling

// All the texts on the screens that are up, as one string: a change means a script is still filling them.
std::wstring text_signature()
{
    std::wstring s;
    for (auto &t : texts_now)
        s += t.second + L'\x1f';
    return s;
}
std::wstring lines_built, lines_said; // the LINE items' texts: a change is a new page of the same box
// "Loading": the game's thread stops taking messages while it loads a screen (15 s on this PC's hard disk
// after Tutorial), so neither it nor the mod can speak. A watcher thread says "Loading" once when the
// ticks stop for over a second right after a press, so the press is known to have landed.
std::atomic<DWORD> tick_at, press_at;
HANDLE watcher, watcher_stop;


int cy(const RECT &r) { return (r.top + r.bottom) / 2; }
int cx(const RECT &r) { return (r.left + r.right) / 2; }

void gather(void *obj, std::vector<El> &out, int &budget)
{
    for (; obj && budget > 0; obj = at<void *>(obj, L.obj_sibling), budget--) {
        if (!at<bool>(obj, L.obj_visible))
            continue; // nor its children
        RECT box = at<RECT>(obj, L.obj_box);
        unsigned kinds = eng::kinds(obj);
        if ((kinds & (eng::BUTTON | eng::TEXT | eng::BAR | eng::LISTBOX | eng::EDITBOX)) && box.right > box.left && box.bottom > box.top &&
            box.right > 0 && box.bottom > 0) {
            El e{obj, kinds, box};
            if (kinds & eng::EDITBOX) {
                e.text = eng::kstring(obj, L.edit_string); // as typed: no markup to strip
                e.tip = plain(eng::kstring(obj, L.obj_tooltip));
                out.push_back(e);
            } else if (kinds & eng::LISTBOX) {
                out.push_back(e); // a list box is a kind of text object: check it first
            } else if (kinds & eng::BUTTON) {
                e.text = plain(eng::kstring(obj, L.button_string));
                e.tip = plain(eng::kstring(obj, L.obj_tooltip));
                out.push_back(e);
            } else if (kinds & eng::TEXT) {
                e.text = plain(eng::kstring(obj, L.text_string));
                // The tutorial's camera controls (scroll, zoom, rotate, tilt with the mouse) mean nothing
                // without the screen (user, 2026-11-07): left out, matched on the game's own string.
                static std::wstring camera;
                if (camera.empty())
                    camera = eng::game_string("IDS_ONCE_ONLY_TUTORIAL_STRING73");
                bool is_camera = camera.size() > 40 && e.text.compare(0, 40, camera, 0, 40) == 0;
                if (!e.text.empty() && !is_camera)
                    out.push_back(e);
            } else {
                out.push_back(e); // progress bar
            }
        }
        gather(at<void *>(obj, L.obj_children), out, budget);
    }
}

bool is_button(const El &e) { return (e.kinds & eng::BUTTON) != 0; }
bool is_bar(const El &e) { return (e.kinds & eng::BAR) != 0; }
bool is_list(const El &e) { return (e.kinds & eng::LISTBOX) != 0; }
bool is_edit(const El &e) { return (e.kinds & eng::EDITBOX) != 0; }
bool indented(void *b) { return at<bool>(b, L.button_indented); }

std::wstring join(const std::vector<El *> &texts)
{
    std::wstring out;
    for (El *t : texts)
        out += (out.empty() ? L"" : L", ") + t->text;
    return out;
}

std::wstring states(void *b)
{
    std::wstring s;
    if (at<bool>(b, L.button_is_checkbox))
        s += at<bool>(b, L.button_checked) ? L", checked" : L", not checked";
    if (at<bool>(b, L.button_indented))
        s += L", selected"; // a tab or option shown pressed in
    if (at<bool>(b, L.button_greyed) || !at<bool>(b, L.obj_enabled))
        s += L", unavailable";
    return s;
}

std::wstring button_label(const El &b)
{
    std::wstring label = b.text.empty() ? b.tip : b.text;
    if (!b.text.empty() && !b.tip.empty() && b.tip != b.text)
        label += L", " + b.tip;
    return label;
}

// A list entry's text; script-drawn lists (the unit list) have none stored: the battle module builds it.
std::wstring entry(void *list, int index)
{
    std::wstring s = plain(eng::list_item(list, index));
    if (s.empty())
        return battle_list_entry(list, index);
    if (eng::name(list) == L"CAMPAIGNNEWLIST") {
        // QuestControlsTools.BSF builds "<name> $ (<expansion>)" for a locked campaign and adds " *" for one
        // that isn't a core campaign; the entry's data is GetCampaignEntry's flags (bit0 locked, bit1 core).
        // The "$" is drawn as a symbol; spoken instead (user, 2026-11-09): the game's locked-content
        // tooltip with the expansion's name, and "user content" for the star.
        unsigned flags = eng::list_item_data(list, index);
        std::wstring pack;
        size_t dollar = s.find(L" $ (");
        if (dollar != std::wstring::npos) {
            size_t close = s.find(L')', dollar);
            pack = s.substr(dollar + 4, close == std::wstring::npos ? std::wstring::npos : close - dollar - 4);
            s.erase(dollar, close == std::wstring::npos ? std::wstring::npos : close + 1 - dollar);
        }
        while (!s.empty() && (s.back() == L'*' || s.back() == L' '))
            s.pop_back();
        while (!s.empty() && s[0] == L' ')
            s.erase(0, 1);
        if (flags & 1) {
            static std::wstring locked = [] {
                std::wstring t = eng::game_string("IDS_LOCKED_EXP_PACK");
                t.erase(std::remove(t.begin(), t.end(), L'~'), t.end());
                while (!t.empty() && (iswspace(t.back()) || t.back() == L':'))
                    t.pop_back();
                return t.empty() ? std::wstring(L"locked") : t;
            }();
            s += L", " + locked + (pack.empty() ? L"" : L": " + pack);
        }
        if (!(flags & 2))
            s += L", user content";
    }
    std::wstring more = battle_force_details(list, index, false); // force selection: cost, quantity
    return more.empty() ? s : s + L", " + more;
}

// The Final Losses table (BattleResults.txt, K or "View Final Losses"): two side panels, each a grid of
// texts (row labels BR_kills1 "Killed"..., column headers BR_enemy_foot "Infantry"..., values
// BRFootKilledText1...). Read in screen order, the sides interleaved row by row (user, 2026-11-09). Here:
// one side after the other, a row per item, each number with its column: "Killed: Infantry 8, Cavalry 0,
// Total 8". Sides by the names (…1 / ENEMY / BR_ = side 1, …0 / FRIENDLY / BR2_ = side 0), left panel first.
std::vector<Item> results_table(std::vector<El> &els)
{
    std::vector<Item> out;
    std::vector<El *> side[2];
    for (El &e : els) {
        if (!(e.kinds & eng::TEXT) || is_button(e) || e.text.empty())
            continue;
        std::wstring n = eng::name(e.obj);
        if (n.compare(0, 2, L"BR") != 0 || n == L"BR_TITLETEXT")
            continue;
        int s = n.compare(0, 4, L"BR2_") == 0 || n.find(L"FRIENDLY") != std::wstring::npos || n.back() == L'0' ? 0
                : n.compare(0, 3, L"BR_") == 0 || n.find(L"ENEMY") != std::wstring::npos || n.back() == L'1' ? 1 : -1;
        if (s >= 0)
            side[s].push_back(&e);
    }
    if (side[0].size() < 4 || side[1].size() < 4)
        return out;
    // As a table (user, 2026-11-09): rows = Killed, Wounded...; columns = each side's Infantry, Cavalry,
    // Total ("Roman Infantry"). The side names and original strengths come first as lines.
    Item table{TABLE, (void *)(uintptr_t)0x9F000000u};
    auto left = [](std::vector<El *> &v) { int m = INT_MAX; for (El *e : v) m = std::min(m, (int)e->box.left); return m; };
    int first = left(side[0]) <= left(side[1]) ? 0 : 1;
    for (int s : {first, 1 - first}) {
        auto &v = side[s];
        for (El *e : v)
            e->used = true;
        std::stable_sort(v.begin(), v.end(), [](El *a, El *b) { return cy(a->box) < cy(b->box); });
        // Rows: texts whose centres are within 10 px up or down.
        std::vector<std::vector<El *>> rows;
        for (El *e : v) {
            if (rows.empty() || abs(cy(e->box) - cy(rows.back()[0]->box)) > 10)
                rows.emplace_back();
            rows.back().push_back(e);
        }
        std::vector<El *> header;
        for (auto &r : rows)
            for (El *e : r) {
                std::wstring n = eng::name(e->obj);
                if (n.find(L"_FOOT") != std::wstring::npos || n.find(L"_HORSE") != std::wstring::npos ||
                    (n.find(L"_TOTAL") != std::wstring::npos && n.find(L"ORIGINAL") == std::wstring::npos))
                    header.push_back(e);
            }
        std::sort(header.begin(), header.end(), [](El *a, El *b) { return a->box.left < b->box.left; });
        if (header.empty())
            return {};
        size_t col0 = table.cols.size();
        std::wstring side_name, caption;
        size_t row = 0;
        for (auto &r : rows) {
            std::sort(r.begin(), r.end(), [](El *a, El *b) { return a->box.left < b->box.left; });
            if (std::find(header.begin(), header.end(), r[0]) != header.end())
                continue; // the column headers row
            if (r.size() == 1) { // the side's name, its original strength
                if (side_name.empty())
                    side_name = r[0]->text;
                caption += (caption.empty() ? L"" : L" ") + r[0]->text;
                continue;
            }
            if (row >= table.rows.size()) {
                table.rows.push_back(r[0]->text);
                table.cells.emplace_back();
            }
            auto &cells = table.cells[row++];
            for (size_t k = 1; k < r.size(); k++) {
                // The value's column: the nearest header; a lone value (Engines Lost) goes under Total.
                size_t c = header.size() - 1;
                if (r.size() > 2) {
                    int best = INT_MAX;
                    for (size_t h = 0; h < header.size(); h++)
                        if (abs(cx(header[h]->box) - cx(r[k]->box)) < best)
                            best = abs(cx(header[h]->box) - cx(r[k]->box)), c = h;
                }
                if (cells.size() < col0 + header.size())
                    cells.resize(col0 + header.size());
                cells[col0 + c] = r[k]->text;
            }
        }
        for (El *h : header)
            table.cols.push_back((side_name.empty() ? L"" : side_name + L" ") + h->text);
        Item it{LINE, (void *)(uintptr_t)(0x90000000u + out.size())}; // apart from the other LINE ids
        it.name = it.speech = caption;
        if (!caption.empty())
            out.push_back(it);
    }
    for (auto &r : table.cells)
        r.resize(table.cols.size());
    table.name = table.speech = L"table, " + std::to_wstring(table.rows.size()) + L" rows, " +
                                std::to_wstring(table.cols.size()) + L" columns";
    if (!table.rows.empty())
        out.push_back(table);
    return out;
}

std::vector<Item> build(std::wstring &title_out)
{
    std::vector<El> els;
    char *ui = *(char **)eng::ui_manager_ptr;
    if (!ui)
        return {};
    int budget = 3000;
    // A popup in battle (unit information) is read alone: the tutorial's box above it is another thing.
    bool popup = false;
    for (int i = 0; i < 24 && !popup; i++) {
        char *screen = *(char **)(ui + L.ui_layers + i * L.layer_size);
        popup = screen && !at<bool>(screen, L.screen_hidden) && battle_popup(screen);
    }
    // From the top layer down, stopping after the first screen that covers
    // what's below (flag 1: Options, the system menu, popups...); the main
    // menu's button overlay (flag 2) lets its background screen through.
    for (int i = 23; i >= 0; i--) {
        char *screen = *(char **)(ui + L.ui_layers + i * L.layer_size);
        if (!screen || at<bool>(screen, L.screen_hidden))
            continue;
        if (battle_owns() && battle_hud(screen + L.screen_name))
            continue; // the battle module reads these
        if (popup && strstr(screen + L.screen_name, "NONMODAL"))
            continue;
        size_t first = els.size();
        gather(at<void *>(screen, L.screen_root), els, budget);
        for (size_t k = first; k < els.size(); k++)
            els[k].layer = i;
        if ((at<unsigned>(screen, L.screen_flags) & 1) || battle_popup(screen))
            break;
    }

    texts_now.clear();
    for (El &e : els)
        if ((e.kinds & eng::TEXT) && !is_button(e))
            texts_now.push_back({e.obj, e.text});
    std::vector<Item> table = results_table(els);

    // Captions: a text whose centre lies on a button names it.
    for (El &t : els) {
        if (!(t.kinds & eng::TEXT) || is_button(t))
            continue;
        for (El &b : els)
            if (is_button(b) && PtInRect(&b.box, POINT{cx(t.box), cy(t.box)})) {
                b.text = b.text.empty() ? t.text : b.text + L", " + t.text;
                t.used = true;
                break;
            }
    }
    // Buttons with nothing to say and not pressed in are decoration or
    // click-blockers; they neither form columns nor become controls (unless a
    // slider row needs them, below).
    // Doubles (user, 2026-11-07): a campaign's details screen lies over the campaign chooser without
    // covering it, both with Go Back / Settings / Load/Save; Load/Save has two Go Back buttons. A button
    // labelled like one already taken is dropped when it is on a lower screen or in the same place
    // (els run top screen first, so the top one stays). Same labels elsewhere on one screen (a "Buy" per
    // row) stay.
    for (size_t a = 0; a < els.size(); a++) {
        if (!is_button(els[a]) || button_label(els[a]).empty())
            continue;
        for (size_t b = 0; b < a; b++)
            if (is_button(els[b]) && !els[b].used && button_label(els[b]) == button_label(els[a]) &&
                (els[b].layer != els[a].layer ||
                 (abs(cx(els[a].box) - cx(els[b].box)) <= 60 && abs(cy(els[a].box) - cy(els[b].box)) <= 60))) {
                els[a].used = true;
                break;
            }
    }
    // The difficulty row (Options.txt OptionsDifficultyButton0-5): six icons without text or tooltip, the chosen one
    // pressed in. Unlabelled buttons count as decoration, so the row's size changed with the choice (4, 5, 6) and
    // the control lost its place; its value text (OptionsDifficultyText) attached to the next control (tester,
    // 2026-11-10). All six count; each is named by its level (UITools.BSF / OPTIONS.BSF: button n = DiffMode n).
    auto difficulty = [](const El &e) {
        std::wstring n = eng::name(e.obj);
        size_t at = n.find(L"DIFFICULTYBUTTON");
        return is_button(e) && at != std::wstring::npos && n.size() == at + 17 && n.back() >= L'0' && n.back() <= L'5';
    };
    for (El &e : els)
        if ((e.kinds & eng::TEXT) && !is_button(e)) {
            std::wstring n = eng::name(e.obj);
            if (n.size() > 14 && n.compare(n.size() - 14, 14, L"DIFFICULTYTEXT") == 0)
                e.used = true; // the chosen level's name: the choice says it
        }
    auto counts = [&](const El &e) { return !e.used && (is_bar(e) || is_list(e) || is_edit(e) || (is_button(e) && (!button_label(e).empty() || indented(e.obj) || difficulty(e)))); };

    // Blocks ("columns"): controls linked when within 40 px sideways and 60 px
    // up or down of each other. Settings has two side by side; a button
    // centred under both (Accept) joins the nearer one instead of fusing them.
    std::vector<El *> ctl;
    for (El &e : els)
        if (counts(e))
            ctl.push_back(&e);
    std::vector<int> parent(ctl.size());
    for (size_t a = 0; a < ctl.size(); a++)
        parent[a] = (int)a;
    auto root = [&](int a) { while (parent[a] != a) a = parent[a] = parent[parent[a]]; return a; };
    auto gap = [](const RECT &a, const RECT &b, bool horizontal) {
        return horizontal ? std::max({(int)(a.left - b.right), (int)(b.left - a.right), 0})
                          : std::max({(int)(a.top - b.bottom), (int)(b.top - a.bottom), 0});
    };
    for (size_t a = 0; a < ctl.size(); a++)
        for (size_t b = a + 1; b < ctl.size(); b++)
            if (gap(ctl[a]->box, ctl[b]->box, true) <= 40 && gap(ctl[a]->box, ctl[b]->box, false) <= 60)
                parent[root((int)a)] = root((int)b);
    struct Block { int root; RECT box; };
    std::vector<Block> blocks;
    for (size_t a = 0; a < ctl.size(); a++) {
        int r = root((int)a);
        auto hit = std::find_if(blocks.begin(), blocks.end(), [&](const Block &b) { return b.root == r; });
        if (hit == blocks.end())
            blocks.push_back({r, ctl[a]->box});
        else
            UnionRect(&hit->box, &hit->box, &ctl[a]->box);
    }
    // Reading order of blocks: top to bottom (tops within 40 px count as level), then left to right.
    std::sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) {
        if (abs(a.box.top - b.box.top) > 40)
            return a.box.top < b.box.top;
        return a.box.left < b.box.left;
    });
    for (size_t a = 0; a < ctl.size(); a++)
        for (size_t c = 0; c < blocks.size(); c++)
            if (blocks[c].root == root((int)a))
                ctl[a]->column = (int)c;
    for (El &e : els) {
        if (e.used || counts(e))
            continue;
        // The block of the nearest control: the main menu's "New free user
        // content" text reaches under the main buttons but sits beside the
        // download button. (Spanning several blocks isn't a test for a title:
        // stray buttons make small blocks of their own.)
        int best = INT_MAX;
        for (El *o : ctl) {
            // Distance, with being above or below weighing more than being to
            // the side, plus how far from level it is: the Music label sits
            // over a small "More Options" icon but is level with the volume
            // slider; "Tribune" sits just under the Difficulty buttons, level
            // with the Combat Log row across the gap.
            int d = gap(e.box, o->box, true) + 4 * gap(e.box, o->box, false) + abs(cy(e.box) - cy(o->box));
            if (d < best)
                best = d, e.column = o->column;
        }
    }
    const int ncols = (int)blocks.size();

    // Lists: the texts in a strip just above one are its column headers.
    std::vector<std::pair<int, Item>> lists;
    for (El &l : els) {
        if (!is_list(l))
            continue;
        std::vector<El *> headers;
        for (El &t : els)
            if (!t.used && (t.kinds & eng::TEXT) && !is_button(t) && t.box.bottom <= l.box.top + 5 &&
                t.box.bottom >= l.box.top - 60 && t.box.left >= l.box.left - 10 && t.box.right <= l.box.right + 10) {
                headers.push_back(&t);
                t.used = true;
            }
        std::sort(headers.begin(), headers.end(), [](El *a, El *b) { return a->box.left < b->box.left; });
        // The campaign and scenario lists (QuestControls.txt): each entry its own menu item, so Up/Down go
        // through them like any menu and each is selected on arrival; the game then shows its description,
        // which is read out (user, 2026-11-09). Same sort key: the stable sort keeps their order.
        std::wstring list_name = eng::name(l.obj);
        if (!at<bool>(l.obj, L.list_dropdown) && (list_name == L"CAMPAIGNNEWLIST" || list_name == L"SCENARIOS_LIST")) {
            int n = eng::list_count(l.obj);
            for (int k = 0; k < n; k++) {
                Item e{ENTRY, (char *)l.obj + 1 + k, {l.obj}}; // identity only: never dereferenced
                e.index = k;
                e.name = e.speech = entry(l.obj, k);
                lists.push_back({l.column * 100000 + l.box.top, e});
            }
            l.used = true;
            continue;
        }
        Item it{LIST, l.obj, {l.obj}};
        it.name = join(headers);
        it.speech = (it.name.empty() ? L"" : it.name + L", ") +
                    (at<bool>(l.obj, L.list_dropdown) ? L"drop-down" : L"list");
        lists.push_back({l.column * 100000 + l.box.top, it});
        l.used = true;
    }
    // Edit boxes (the save name): labelled by a text level with it on its left, or just above it.
    for (El &ed : els) {
        if (!is_edit(ed))
            continue;
        El *label = nullptr;
        int best = INT_MAX;
        for (El &t : els) {
            if (t.used || !(t.kinds & eng::TEXT) || is_button(t) || is_list(t))
                continue;
            bool left = abs(cy(t.box) - cy(ed.box)) <= 20 && t.box.right <= ed.box.left + 5 && ed.box.left - t.box.right <= 300;
            bool above = t.box.bottom <= ed.box.top + 5 && ed.box.top - t.box.bottom <= 40 &&
                         t.box.right > ed.box.left && t.box.left < ed.box.right;
            int d = left ? ed.box.left - t.box.right : above ? ed.box.top - t.box.bottom : INT_MAX;
            if (d < best)
                best = d, label = &t;
        }
        Item it{EDIT, ed.obj};
        if (label) {
            it.name = label->text;
            label->used = true;
        } else {
            it.name = ed.tip;
        }
        it.text = ed.text;
        it.speech = (it.name.empty() ? L"" : it.name + L", ") + L"edit" + (ed.text.empty() ? L"" : L", " + ed.text);
        lists.push_back({ed.column * 100000 + ed.box.top, it});
        ed.used = true;
    }

    struct Heading { int column, y; std::wstring text; };
    std::vector<Heading> headings;
    std::vector<std::pair<int, Item>> placed = lists; // (column*100000 + y, item) for ordering
    for (El &e : els)
        if (!e.used && e.column < 0 && (e.kinds & eng::TEXT))
            headings.push_back({-1, e.box.top, e.text});

    for (int c = 0; c < ncols; c++) {
        std::vector<El *> col;
        for (El &e : els)
            if (!e.used && e.column == c)
                col.push_back(&e);
        std::sort(col.begin(), col.end(), [](El *a, El *b) { return cy(a->box) < cy(b->box); });
        Item *last_item = nullptr;
        int last_bottom = INT_MIN;
        for (size_t i = 0; i < col.size();) {
            // One row: elements whose centres are within 20 px of the first's.
            size_t j = i;
            std::vector<El *> texts, buttons, bars;
            int bottom = INT_MIN, top = INT_MAX;
            for (; j < col.size() && cy(col[j]->box) - cy(col[i]->box) <= 20; j++) {
                El *e = col[j];
                (is_bar(*e) ? bars : is_button(*e) ? buttons : texts).push_back(e);
                bottom = std::max(bottom, (int)e->box.bottom);
                top = std::min(top, (int)e->box.top);
            }
            std::sort(buttons.begin(), buttons.end(), [](El *a, El *b) { return a->box.left < b->box.left; });
            std::sort(texts.begin(), texts.end(), [](El *a, El *b) { return a->box.left < b->box.left; });
            int key = c * 100000 + top;
            bool any_indented = false;
            for (El *b : buttons)
                any_indented |= indented(b->obj);
            size_t before = placed.size();

            if (!bars.empty() && buttons.size() >= 2) {
                Item it{SLIDER, bars[0]->obj, {buttons.front()->obj, buttons.back()->obj}};
                it.name = join(texts);
                if (it.name.empty())
                    it.name = buttons.front()->tip;
                it.speech = it.name + L", " + std::to_wstring((at<int>(bars[0]->obj, L.bar_progress) + 500) / 1000) + L"%"; // thousandths of a percent
                placed.push_back({key, it});
            } else if (any_indented && buttons.size() >= 2) {
                Item it{CHOICE, buttons[0]->obj};
                it.name = join(texts);
                std::wstring value;
                for (size_t k = 0; k < buttons.size(); k++) {
                    it.choices.push_back(buttons[k]->obj);
                    if (indented(buttons[k]->obj)) {
                        value = buttons[k]->text.empty() ? buttons[k]->tip : buttons[k]->text;
                        // The game's description of the choice (its tooltip, "Turn on detailed popup close
                        // combat reports") after a value that is the button's text (tester, 2026-11-10).
                        if (!buttons[k]->text.empty() && !buttons[k]->tip.empty() && buttons[k]->tip != buttons[k]->text)
                            value += L", " + buttons[k]->tip;
                        std::wstring n = eng::name(buttons[k]->obj);
                        static const char *const levels[] = {"IDS_DIFFMODE_NORMAL", "IDS_DIFFMODE_HELPER", "IDS_DIFFMODE_FUN",
                                                             "IDS_DIFFMODE_HARD", "IDS_DIFFMODE_VERYHARD", "IDS_DIFFMODE_NIGHTMARE"};
                        if (value.empty() && n.find(L"DIFFICULTYBUTTON") != std::wstring::npos && n.back() >= L'0' && n.back() <= L'5')
                            value = eng::game_string(levels[n.back() - L'0']);
                        if (value.empty())
                            value = std::to_wstring(k + 1) + L" of " + std::to_wstring(buttons.size());
                    }
                }
                it.speech = (it.name.empty() ? L"" : it.name + L", ") + value;
                placed.push_back({key, it});
            } else {
                std::vector<El *> labelled;
                for (El *b : buttons)
                    if (!button_label(*b).empty())
                        labelled.push_back(b);
                for (El *b : labelled) {
                    Item it{BUTTON, b->obj};
                    it.name = button_label(*b);
                    placed.push_back({key + (int)b->box.left / 10, it});
                }
                if (labelled.size() == 1 && !texts.empty()) {
                    // The row's text names its only button ("Game Speed").
                    Item &it = placed.back().second;
                    it.name = join(texts) + L", " + it.name;
                } else if (labelled.size() > 1) {
                    for (El *t : texts) {
                        // Only a text right beside a button belongs to it; a title
                        // level with a row of icons is a heading.
                        bool beside = false;
                        for (El *b : labelled)
                            beside |= std::max({(int)(t->box.left - b->box.right), (int)(b->box.left - t->box.right), 0}) <= 40;
                        if (!beside) {
                            headings.push_back({c, t->box.top, t->text});
                            continue;
                        }
                        // Join the nearest button: "New free user content..." -> the download button.
                        size_t best = before;
                        int best_gap = INT_MAX;
                        for (size_t k = before; k < placed.size(); k++) {
                            RECT b = at<RECT>(placed[k].second.obj, L.obj_box);
                            int gap = std::max({(int)(t->box.left - b.right), (int)(b.left - t->box.right), 0});
                            if (gap < best_gap)
                                best_gap = gap, best = k;
                        }
                        placed[best].second.name += L", " + t->text;
                    }
                }
                for (size_t k = before; k < placed.size(); k++)
                    placed[k].second.speech = placed[k].second.name + states(placed[k].second.obj);
            }

            if (placed.size() > before) {
                last_item = &placed.back().second;
                last_bottom = bottom;
            } else if (!texts.empty()) {
                if (last_item && top - last_bottom <= 8) {
                    // A value shown just under its control ("Tribune" under Difficulty).
                    std::wstring value = join(texts);
                    if (last_item->type == CHOICE) {
                        // A placeholder "2 of 6" (no name, no button text) gives way to the value; a choice
                        // without a name takes the text right above it (the end-of-battle "Difficulty").
                        size_t comma = last_item->speech.rfind(L", ");
                        size_t from = comma == std::wstring::npos ? 0 : comma + 2;
                        bool numbered = last_item->speech.find(L" of ", from) != std::wstring::npos;
                        std::wstring head = comma == std::wstring::npos ? L"" : last_item->speech.substr(0, comma);
                        if (last_item->name.empty() && !headings.empty()) {
                            RECT box = at<RECT>(last_item->obj, L.obj_box);
                            if (headings.back().y < box.top && box.top - headings.back().y <= 120) {
                                last_item->name = headings.back().text;
                                headings.pop_back();
                                head = last_item->name;
                            }
                        }
                        last_item->speech = numbered ? (head.empty() ? L"" : head + L", ") + value
                                                     : last_item->speech + L", " + value;
                    } else {
                        last_item->speech += L", " + value;
                    }
                    last_bottom = bottom;
                } else {
                    headings.push_back({c, top, join(texts)});
                }
            }
            i = j;
        }
    }

    std::stable_sort(placed.begin(), placed.end(), [](auto &a, auto &b) { return a.first < b.first; });
    int top_y = INT_MAX;
    title_out.clear();
    for (auto &h : headings)
        if (h.y < top_y)
            top_y = h.y, title_out = h.text;
    std::vector<Item> out;
    for (auto &[key, it] : placed) {
        int column = key / 100000, y = key % 100000;
        int best_y = INT_MIN;
        for (auto &h : headings)
            if ((h.column == column || h.column < 0) && h.y <= y && h.y > best_y) {
                best_y = h.y;
                it.heading = h.text;
            }
        out.push_back(it);
    }
    // Headings only when the screen has several; a lone one is just the
    // title, spoken when the screen opens.
    std::vector<std::wstring> used;
    for (Item &it : out)
        if (!it.heading.empty() && std::find(used.begin(), used.end(), it.heading) == used.end())
            used.push_back(it.heading);
    static size_t logged_count;
    if (used.size() != logged_count) {
        logged_count = used.size();
        std::wstring all;
        for (auto &h : used)
            all += L" [" + h + L"]";
        logf("headings:%ls", all.c_str());
    }
    if (used.size() < 2) {
        for (Item &it : out)
            it.heading.clear();
        // No sections: one-line texts are the title, spoken when the screen opens; a text of
        // several lines (an information popup's description, a tutorial box) becomes LINE items
        // ahead of the controls, read with the arrows.
        std::stable_sort(headings.begin(), headings.end(), [](auto &a, auto &b) { return a.y < b.y; });
        title_out.clear();
        std::vector<Item> lines;
        for (auto &h : headings) {
            if (h.text.find(L'\n') == std::wstring::npos) {
                title_out += (title_out.empty() ? L"" : L". ") + h.text;
                continue;
            }
            for (size_t a = 0; a <= h.text.size();) {
                size_t b = std::min(h.text.find(L'\n', a), h.text.size());
                std::wstring line = h.text.substr(a, b - a);
                while (!line.empty() && iswspace(line.back()))
                    line.pop_back();
                while (!line.empty() && iswspace(line.front()))
                    line.erase(0, 1);
                // A sentence the screen wraps mid-way ("...that this was a" / "DEFEAT", the end-of-battle
                // box) is one item: the previous line of this text ends in a lowercase letter or a comma
                // and this one starts lowercase or is all capitals. Lists ("Men: 120") don't match.
                bool upper = !line.empty();
                for (wchar_t c : line)
                    if (iswlower(c))
                        upper = false;
                bool wrapped = !line.empty() && !lines.empty() && a > 0 && (iswlower(line[0]) || upper) &&
                               (iswlower(lines.back().speech.back()) || lines.back().speech.back() == L',');
                if (wrapped) {
                    lines.back().speech += L" " + line;
                    lines.back().name = lines.back().speech;
                } else if (!line.empty() && !lines.empty() && lines.back().speech.back() == L':') {
                    lines.back().speech += L" " + line; // "Shooting:" and its "None" are one item
                    lines.back().name = lines.back().speech;
                } else if (!line.empty()) {
                    Item it{LINE, (void *)(uintptr_t)(0x80000000u + lines.size())}; // by position: never dereferenced
                    it.name = it.speech = line;
                    lines.push_back(it);
                }
                a = b + 1;
            }
        }
        out.insert(out.begin(), lines.begin(), lines.end());
    }
    out.insert(out.begin(), table.begin(), table.end());
    lines_built.clear();
    for (Item &it : out)
        if (it.type == LINE)
            lines_built += it.speech + L"\n";
    return out;
}

size_t index_of(void *obj)
{
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].obj == obj)
            return i;
    return SIZE_MAX;
}

void watch_texts();

void announce(size_t i, bool heading, bool opening = false)
{
    settle_at = 0;
    if (items[i].type == TABLE && current != items[i].obj)
        table_row = table_col = 0; // coming into the table: its first cell
    if (items[i].type == ENTRY && at<int>(items[i].choices[0], L.list_selected) != items[i].index) {
        watch_texts(); // the game shows the entry's description: read it after the name
        eng::list_select(items[i].choices[0], items[i].index);
    }
    current = items[i].obj;
    std::wstring s;
    if (heading && !items[i].heading.empty())
        s = items[i].heading + L", ";
    else if (heading && (i == 0 || opening) && !title.empty())
        s = title + L", "; // a new screen: its title even when the first control is above it
    s += items[i].speech;
    if (items[i].type == TABLE) {
        const Item &t = items[i];
        table_row = std::min(table_row, (int)t.rows.size() - 1);
        table_col = std::min(table_col, (int)t.cols.size() - 1);
        const std::wstring &v = t.cells[table_row][table_col];
        s += L", " + t.rows[table_row] + L", " + t.cols[table_col] + L", " + (v.empty() ? L"blank" : v);
    }
    if (items[i].type == LIST) {
        void *list = items[i].obj;
        int n = eng::list_count(list), sel = at<int>(list, L.list_selected);
        if (sel >= 0 && sel < n)
            s += L", " + entry(list, sel) + L", " + std::to_wstring(sel + 1) + L" of " + std::to_wstring(n);
        else
            s += L", " + std::to_wstring(n) + L" items";
    }
    s += L", " + std::to_wstring(i + 1) + L" of " + std::to_wstring(items.size());
    speak(s);
}

// After the user picks something, read the texts it changed (queued after
// what was already said): a scenario's description, a tab's contents.
void watch_texts()
{
    texts_before = texts_now;
    watch_until = GetTickCount() + 1500;
}

// Move to another control; its heading is spoken when it differs from the one we left.
void move_to(size_t from, size_t to)
{
    announce(to, from >= items.size() || items[from].heading != items[to].heading);
}

// Timer: speak a new screen, or the current control's new value after a press.
void refresh()
{
    std::wstring fresh_title;
    std::vector<Item> fresh = build(fresh_title);
    bool new_title = fresh_title != title;
    title = fresh_title;
    bool same = fresh.size() == items.size();
    for (size_t i = 0; same && i < fresh.size(); i++)
        same = fresh[i].obj == items[i].obj;
    size_t old = index_of(current);
    std::wstring old_speech = old != SIZE_MAX ? items[old].speech : L"";
    std::wstring old_text = old != SIZE_MAX ? items[old].text : L"";
    items.swap(fresh);
    // A new screen is announced as a whole. Controls coming or going on the same screen (Autofill
    // shows a Decrease Force button) keep the watch: the control we're on is still there.
    if (!same && index_of(current) == SIZE_MAX)
        watch_until = 0;
    if (watch_until && GetTickCount() < watch_until) {
        for (auto &[obj, text] : texts_now) {
            auto old_text = std::find_if(texts_before.begin(), texts_before.end(), [&](auto &b) { return b.first == obj; });
            // A changed text a control already says (the difficulty's name: "Tribune" then "Difficulty,
            // Tribune", 2026-11-10) is left to the control.
            bool said_by_control = false;
            for (const Item &it : items) // already the fresh ones; LINE items are a text's own lines
                if (it.type != LINE && !text.empty() && it.speech.find(text) != std::wstring::npos)
                    said_by_control = true;
            if ((old_text == texts_before.end() || old_text->second != text) && !said_by_control)
                speak(text, false);
        }
        texts_before = texts_now;
    }
    if (items.empty()) {
        // Text with nothing to press (campaign text overlay, a timed pop-up that goes away by itself,
        // loading tips): say it when it appears or changes (user, 2026-11-07).
        if (new_title && !title.empty())
            speak(title, false);
        return;
    }
    size_t now = index_of(current);
    if (!same && now == SIZE_MAX) {
        // A new screen (or a submenu replacing the controls): its script fills the texts over the
        // next frames, so it is spoken once they have stopped changing for 50 ms (60 ms at the least,
        // half a second at the most). A fixed 60 ms read the unit information popup's title before the
        // game had replaced the previous unit's (user, 2026-11-10: Ctrl+Enter on unit 2 said unit 1's name).
        settle_at = GetTickCount() + 60;
        settle_deadline = GetTickCount() + 500;
        settle_sig = text_signature();
        return;
    }
    if (settle_at) {
        std::wstring sig = text_signature();
        if (sig != settle_sig) {
            settle_sig = sig;
            settle_at = std::max(settle_at, GetTickCount() + 50);
        }
        if (GetTickCount() >= std::min(settle_at, settle_deadline)) {
            lines_said = lines_built;
            // A screen with a list (campaigns, scenarios, saves, the unit list) opens on the list, the
            // thing it is for (user, 2026-11-07); a tutorial box's lines still come first.
            size_t start = 0;
            // Campaigns / scenarios: on the selected entry, ahead of the description lines above it.
            bool on_entry = false;
            for (size_t k = 0; k < items.size() && !on_entry; k++)
                if (items[k].type == ENTRY && items[k].index == at<int>(items[k].choices[0], L.list_selected))
                    start = k, on_entry = true;
            if (!on_entry && items[0].type != LINE)
                for (size_t k = 0; k < items.size(); k++)
                    if (items[k].type == LIST && !at<bool>(items[k].obj, L.list_dropdown)) {
                        start = k;
                        break;
                    }
            announce(start, true, true);
        }
        return;
    }
    if (lines_built != lines_said) {
        // The same controls with a new text: the next tutorial page. Start reading it from the top.
        // Not when it changed because the user picked another list entry (a campaign's description,
        // 2026-11-09: it pulled the user out of the campaign list): the watch above reads it out.
        size_t on = index_of(current);
        bool picking = on != SIZE_MAX && (items[on].type == ENTRY || items[on].type == LIST);
        lines_said = lines_built;
        if (picking)
            return;
        watch_until = 0;
        if (!lines_built.empty()) {
            announce(0, false);
            return;
        }
    }
    if (now != SIZE_MAX && old != SIZE_MAX && items[now].type == EDIT && items[now].speech != old_speech) {
        // Typing: speak what was added or deleted, not the whole box again.
        const std::wstring &a = items[now].text, &b = old_text;
        if (a.size() > b.size() && a.compare(0, b.size(), b) == 0)
            speak(a.substr(b.size()));
        else if (b.size() > a.size() && b.compare(0, a.size(), a) == 0)
            speak(b.substr(a.size()));
        else
            speak(a.empty() ? L"blank" : a);
        return;
    }
    if (now != SIZE_MAX && new_title && !title.empty() && !(watch_until && GetTickCount() < watch_until))
        speak(title, false); // the same controls with new text: the next tutorial page
    else if (now != SIZE_MAX && items[now].speech != old_speech)
        announce(now, false); // only a real change: other screens coming and going don't repeat it
}

bool editing()
{
    char *ui = *(char **)eng::ui_manager_ptr;
    void *focus = ui ? at<void *>(ui, L.ui_focus) : nullptr;
    return focus && (eng::kinds(focus) & eng::EDITBOX);
}

void press(void *button)
{
    logf("pressing %ls", eng::name(button).c_str());
    press_at = GetTickCount();
    watch_texts();
    eng::trigger(button);
}

// Choice rows: press the choice `step` away from the selected one.
void step_choice(const Item &it, int step, bool wrap)
{
    int n = (int)it.choices.size(), sel = 0;
    for (int k = 0; k < n; k++)
        if (indented(it.choices[k]))
            sel = k;
    int to = sel + step;
    if (wrap)
        to = (to + n) % n;
    if (to >= 0 && to < n && to != sel)
        press(it.choices[to]);
}

// Where typing c leads among names, starting at i: the same letter again moves
// to the next name starting with it; otherwise the first name (from i on)
// starting with all that was typed. SIZE_MAX if none.
size_t prefix_search(wchar_t c, size_t i, const std::vector<std::wstring> &names)
{
    DWORD now = GetTickCount();
    if (now - typed_at > 1000)
        typed.clear();
    typed_at = now;
    typed += c;
    bool repeat = typed.size() > 1 && typed.find_first_not_of(typed[0]) == std::wstring::npos;
    std::wstring prefix = repeat ? typed.substr(0, 1) : typed;
    size_t n = names.size(), start = (repeat || typed.size() == 1) ? 1 : 0;
    for (size_t step = start; step < n + start; step++) {
        size_t k = (i + step) % n;
        const std::wstring &name = names[k];
        if (name.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), name.begin(), [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }))
            return k;
    }
    return SIZE_MAX;
}

// Lists and drop-downs: move the selection the way clicking an entry does.
// A table: Up/Down rows (past the first or last row the menu moves on), Left/Right columns, Home/End the
// row's first / last column. A new row says its header, a new column its header, then the cell.
bool table_key(const Item &t, WPARAM vk, bool act)
{
    int r = table_row, c = table_col, rows = (int)t.rows.size(), cols = (int)t.cols.size();
    switch (vk) {
    case VK_UP: if (r == 0) return false; r--; break;
    case VK_DOWN: if (r + 1 >= rows) return false; r++; break;
    case VK_LEFT: c = std::max(c - 1, 0); break;
    case VK_RIGHT: c = std::min(c + 1, cols - 1); break;
    case VK_HOME: c = 0; break;
    case VK_END: c = cols - 1; break;
    default: return false;
    }
    if (!act)
        return true;
    std::wstring s;
    if (r != table_row)
        s = t.rows[r] + L", ";
    if (c != table_col)
        s += t.cols[c] + L", ";
    if (r == table_row && c == table_col)
        s = L"edge, "; // as the battle grid says at its edge
    table_row = r;
    table_col = c;
    const std::wstring &v = t.cells[r][c];
    speak(s + (v.empty() ? L"blank" : v));
    return true;
}

bool list_key(const Item &it, WPARAM vk, bool act)
{
    void *list = it.obj;
    int n = eng::list_count(list), sel = at<int>(list, L.list_selected), to;
    if (n <= 0)
        return false;
    // Force selection (ForceOverlay, ForceSelect_ListBox.BSF): its script buys one of the type under the
    // mouse while the left button is down and sells one with the right. Right / Left call that script's
    // UI_OBJ_UPDATE(mousex, mousey, buttons, over, dragging, flags, data) for the selected entry the same
    // way, and the points box it updates is spoken.
    if ((vk == VK_LEFT || vk == VK_RIGHT) && eng::name(list) == L"FORCEOVLY_LIST") {
        if (act && sel >= 0) {
            RECT box = at<RECT>(list, L.obj_box);
            int args[7] = {box.left + 1, box.top + 1, vk == VK_RIGHT ? 1 : 2, 1, 0, 0, sel};
            watch_texts();
            if (!eng::call_script(list, "UI_OBJ_UPDATE", args, 7))
                logf("force selection: no UI_OBJ_UPDATE");
            else
                speak(battle_force_details(list, sel, true)); // the new quantity, then the points box
        }
        return true;
    }
    switch (vk) {
    case VK_DOWN: to = std::min(sel + 1, n - 1); break;
    case VK_UP: to = std::max(sel - 1, 0); break;
    case VK_HOME: to = 0; break;
    case VK_END: to = n - 1; break;
    case VK_NEXT: to = std::min(sel + 10, n - 1); break;
    case VK_PRIOR: to = std::max(sel - 10, 0); break;
    default: {
        wchar_t c = (wchar_t)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR);
        if (!iswalnum(c))
            return false;
        if (!act)
            return true;
        std::vector<std::wstring> names;
        for (int k = 0; k < n; k++)
            names.push_back(entry(list, k));
        size_t found = prefix_search(c, sel < 0 ? 0 : sel, names);
        if (found == SIZE_MAX)
            return true;
        to = (int)found;
    }
    }
    if (!act)
        return true;
    if (to != sel) {
        watch_texts();
        eng::list_select(list, to);
    }
    speak(entry(list, to) + L", " + std::to_wstring(to + 1) + L" of " + std::to_wstring(n));
    return true;
}

void type_search(wchar_t c, size_t i)
{
    std::vector<std::wstring> names;
    for (auto &it : items)
        names.push_back(it.name);
    size_t k = prefix_search(c, i, names);
    if (k != SIZE_MAX && k != i)
        move_to(i, k);
}

// Escape steps out: press the screen's own way out (Close, Go Back, Back, Cancel, OK...), found by
// the button's object name or its text. In battle with only the tutorial's box up, Escape stays the
// battle's / the game's (the system menu).
bool escape(bool act)
{
    if (battle_owns())
        return false;
    static const wchar_t *const names[] = {L"CLOSE", L"BACK", L"CANCEL", L"OK", L"EXIT", L"DONE"};
    static const wchar_t *const words[] = {L"close", L"go back", L"back", L"cancel", L"ok", L"exit", L"done"};
    for (const wchar_t *want : names)
        for (Item &it : items) {
            if (it.type != BUTTON)
                continue;
            std::wstring n = eng::name(it.obj);
            size_t at_end = n.size() >= wcslen(want) ? n.size() - wcslen(want) : std::wstring::npos;
            if (at_end != std::wstring::npos && n.compare(at_end, std::wstring::npos, want) == 0 ||
                n.find(std::wstring(want) + L"BUTTON") != std::wstring::npos) {
                if (act)
                    press(it.obj);
                return true;
            }
        }
    for (const wchar_t *want : words)
        for (Item &it : items) {
            std::wstring label = it.name;
            for (wchar_t &c : label)
                c = towlower(c);
            if (it.type == BUTTON && label == want) {
                if (act)
                    press(it.obj);
                return true;
            }
        }
    return false;
}

// The screen's way in for its list (user, 2026-11-07): Enter on a scenario or campaign starts it with
// the button a sighted player clicks after picking one (SCENARIOS_ACCEPT, "Launch"). Found by the
// object's name ending, else its label; nullptr if the screen has none.
void *start_button()
{
    static const wchar_t *const names[] = {L"ACCEPT", L"LAUNCH", L"START", L"PLAY"};
    static const wchar_t *const words[] = {L"accept", L"launch", L"start", L"play"};
    for (const wchar_t *want : names)
        for (Item &it : items) {
            std::wstring n = eng::name(it.obj);
            size_t len = wcslen(want);
            if (it.type == BUTTON && n.size() >= len && n.compare(n.size() - len, len, want) == 0)
                return it.obj;
        }
    for (const wchar_t *want : words)
        for (Item &it : items) {
            if (it.type != BUTTON)
                continue;
            std::wstring label = it.name.substr(0, it.name.find(L','));
            for (wchar_t &c : label)
                c = towlower(c);
            if (label == want)
                return it.obj;
        }
    return nullptr;
}

// Handle a key; true if it was ours. act=false only asks (the hook is also
// called for keys that are only being peeked at).
bool key(WPARAM vk, bool act)
{
    if (items.empty() || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
        return false;
    // On the battle map Tab is the game's "next unit that hasn't moved" (user, 2026-11-07): it used to
    // jump into the tutorial box's lines instead. That box stays on Up/Down, Space presses Proceed.
    if (vk == VK_TAB && battle_owns() && !editing())
        return false;
    size_t n = items.size(), i = index_of(current);
    if (i == SIZE_MAX)
        i = 0;
    const Item &it = items[i];
    if (editing()) {
        // Typing a name: the edit box keeps its keys (Enter too: the screen's script may act on it).
        // Tab / Up / Down leave the box and move on; Escape just leaves it.
        if (vk != VK_TAB && vk != VK_UP && vk != VK_DOWN && vk != VK_ESCAPE)
            return false;
        if (act) {
            eng::clear_focus();
            if (vk == VK_ESCAPE)
                announce(i, false);
        }
        if (vk == VK_ESCAPE)
            return true;
    }
    if (it.type == LIST && vk != VK_TAB && list_key(it, vk, act))
        return true;
    if (it.type == TABLE && table_key(it, vk, act))
        return true;
    switch (vk) {
    case VK_DOWN: if (act) move_to(i, i + 1 < n ? i + 1 : i); return true;
    case VK_UP: if (act) move_to(i, i > 0 ? i - 1 : 0); return true;
    case VK_TAB: if (act) move_to(i, GetKeyState(VK_SHIFT) < 0 ? (i + n - 1) % n : (i + 1) % n); return true;
    case VK_ESCAPE:
        return escape(act);
    case VK_HOME: if (act) move_to(i, 0); return true;
    case VK_END: if (act) move_to(i, n - 1); return true;
    case VK_RETURN:
    case VK_SPACE:
        if (act) {
            int sel = it.type == LIST ? at<int>(it.obj, L.list_selected) : -1;
            if (it.type == LIST && sel >= 0 && battle_list_pick(it.obj, sel))
                escape(true); // the unit is selected: close the list (its Close button) back to the map
            else if (it.type == ENTRY) {
                if (void *start = start_button()) // Launch / Accept, for the entry selected on arrival
                    press(start);
            } else if (it.type == LIST && sel >= 0 && !at<bool>(it.obj, L.list_dropdown)) {
                if (void *start = start_button())
                    press(start);
            } else if (it.type == BUTTON)
                press(it.obj); // a LINE does nothing
            else if (it.type == EDIT) {
                eng::focus(it.obj);
                speak(editing() ? L"editing" : L"unavailable");
            } else if (it.type == CHOICE)
                step_choice(it, 1, true);
        }
        return true;
    case VK_LEFT:
    case VK_RIGHT:
        if (it.type == BUTTON || it.type == LINE || it.type == EDIT || it.type == ENTRY)
            return false;
        if (act) {
            if (it.type == SLIDER)
                press(it.choices[vk == VK_LEFT ? 0 : 1]);
            else
                step_choice(it, vk == VK_LEFT ? -1 : 1, false);
        }
        return true;
    }
    wchar_t c = (wchar_t)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR);
    if (!iswalnum(c))
        return false;
    if (act)
        type_search(c, i);
    return true;
}

// After a caught fault (a screen freed under us while the game rebuilds its screens, as when a module is
// switched): forget the screen and its objects, and leave the game alone for half a second.
DWORD pause_until;

void forget_screen()
{
    items.clear();
    current = nullptr;
    title.clear();
    texts_now.clear();
    texts_before.clear();
    lines_built.clear();
    lines_said.clear();
    watch_until = 0;
    settle_at = 0;
    pause_until = GetTickCount() + 500;
}

LRESULT CALLBACK on_message(int code, WPARAM remove, LPARAM lp)
{
    busy++;
    MSG *msg = (MSG *)lp;
    if (code == HC_ACTION && remove == PM_REMOVE) {
        if (msg->message == MSG_START && !msg->hwnd) {
            timer = SetTimer(nullptr, 0, 16, nullptr);
            if (ready)
                battle_start();
            logf("menus: timer %s on thread %lu", timer ? "started" : "failed", GetCurrentThreadId());
            msg->message = WM_NULL;
        } else if (msg->message == MSG_STOP && !msg->hwnd) {
            battle_stop();
            if (timer)
                KillTimer(nullptr, timer);
            timer = 0;
            SetEvent(stopped);
            msg->message = WM_NULL;
        } else if (msg->message == WM_TIMER && !msg->hwnd && msg->wParam == timer && timer) {
            if (ready && GetTickCount() >= pause_until) {
                // Timing: a slow tick would delay the game's own frame; log any over 4 ms.
                LARGE_INTEGER t0, t1, f;
                QueryPerformanceCounter(&t0);
                if (!eng::guarded([](void *) { battle_tick(); }, nullptr) ||
                    !eng::guarded([](void *) { refresh(); }, nullptr))
                    forget_screen();
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&f);
                double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
                if (ms > 4)
                    logf("slow tick: %.1f ms", ms);
                static DWORD last_tick;
                DWORD now = GetTickCount();
                if (last_tick && now - last_tick > 1000)
                    logf("no tick for %lu ms (the game wasn't taking messages)", now - last_tick);
                last_tick = now;
                tick_at = now;
            }
            msg->message = WM_NULL;
        }
    }
    if (code == HC_ACTION && (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) && ready &&
        GetTickCount() >= pause_until) {
        struct KeyCall { MSG *msg; bool act, took; } call{msg, remove == PM_REMOVE, false};
        bool ok = eng::guarded([](void *p) {
            KeyCall &c = *(KeyCall *)p;
            if (editing()) {
                // A name being typed (the save box, also from the battle's system menu): letters are text, not battle keys.
                c.took = c.msg->message == WM_KEYDOWN && key(c.msg->wParam, c.act);
            } else {
                c.took = battle_key(c.msg->wParam, c.act) ||
                         (c.msg->message == WM_KEYDOWN && key(c.msg->wParam, c.act)) || battle_blocks(c.msg->wParam);
            }
        }, &call);
        if (!ok)
            forget_screen();
        else if (call.took)
            msg->message = WM_NULL;
    }
    LRESULT r = CallNextHookEx(nullptr, code, remove, lp);
    busy--;
    return r;
}

// The "Loading" watcher (its own thread: the game's is the one that's stuck). Speaks once per stall.
DWORD WINAPI watch_loading(void *)
{
    DWORD said_for = 0;
    while (WaitForSingleObject(watcher_stop, 200) == WAIT_TIMEOUT) {
        DWORD t = tick_at, p = press_at, now = GetTickCount();
        if (t && p && p + 50 >= t && now - t > 1000 && said_for != t) {
            said_for = t;
            speak(L"Loading");
        }
    }
    return 0;
}

} // namespace

void menus_start(const std::wstring &dir)
{
    HMODULE loader = GetModuleHandleW(L"version.dll");
    auto main_thread = loader ? (DWORD(*)(void))GetProcAddress(loader, "loader_main_thread") : nullptr;
    if (!main_thread)
        return;
    game_thread = main_thread();
    ready = eng::init(dir);
    eng::guard_start();
    stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&on_message, &self);
    hook = SetWindowsHookExW(WH_GETMESSAGE, on_message, self, game_thread);
    logf("menus: key hook %s", hook ? "installed" : "failed");
    if (hook)
        PostThreadMessageW(game_thread, MSG_START, 0, 0);
    tick_at = 0;
    press_at = 0;
    watcher_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watcher = CreateThread(nullptr, 0, watch_loading, nullptr, 0, nullptr);
}

void menus_stop()
{
    if (watcher) {
        SetEvent(watcher_stop);
        WaitForSingleObject(watcher, 1000);
        CloseHandle(watcher);
        CloseHandle(watcher_stop);
    }
    watcher = watcher_stop = nullptr;
    if (hook) {
        // The timer belongs to the game thread: ask it to stop it, then unhook.
        PostThreadMessageW(game_thread, MSG_STOP, 0, 0);
        WaitForSingleObject(stopped, 500);
        UnhookWindowsHookEx(hook);
    }
    hook = nullptr;
    while (busy > 0)
        Sleep(1);
    eng::guard_stop(); // before the DLL goes: the handler is in it
    battle_free();
    if (stopped)
        CloseHandle(stopped);
    stopped = nullptr;
    items.clear();
    current = nullptr;
    ready = false;
}
