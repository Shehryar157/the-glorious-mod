#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <unordered_map>

#include "engine.h"
#include "speech.h"

namespace eng {

Layout L;
uintptr_t ui_manager_ptr;
int trigger_slot = -1;
uintptr_t game_ptr, battle_map_ptr, side_data_ptr, terrain_table, terrain_count_ptr, fn_do_pick, fn_handle_mouse, fn_select_unit, fn_do_action;
uintptr_t mouse_ptr, block_manager_ptr, fn_control_under_mouse, fn_over_block, fn_minimap_pos;
uintptr_t fn_fixed_height;
static uintptr_t fn_request_focus, fn_clear_focus;
static uintptr_t fn_item_count, fn_set_selected, fn_message, fn_find_entry, string_store_ptr, fn_get_function,
    fn_call_function;

// --- symbol lookup through dbghelp (the copy the game ships and loads) ---

typedef BOOL(WINAPI *SymInitializeW_t)(HANDLE, PCWSTR, BOOL);
typedef DWORD(WINAPI *SymSetOptions_t)(DWORD);
typedef DWORD64(WINAPI *SymLoadModuleExW_t)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
typedef BOOL(WINAPI *SymFromNameW_t)(HANDLE, PCWSTR, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetTypeFromNameW_t)(HANDLE, ULONG64, PCWSTR, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetTypeInfo_t)(HANDLE, DWORD64, ULONG, IMAGEHLP_SYMBOL_TYPE_INFO, PVOID);
typedef BOOL(WINAPI *SymCleanup_t)(HANDLE);

enum { TAG_DATA = 7, TAG_BASECLASS = 18 }; // DIA SymTagEnum values (cvconst.h, not in mingw)

static const HANDLE SESSION = (HANDLE)0xF0A2; // our own dbghelp session, apart from the game's
static const DWORD64 BASE = 0x400000;         // the exe has no ASLR
static SymFromNameW_t sym_from_name;
static SymGetTypeFromNameW_t sym_type_from_name;
static SymGetTypeInfo_t sym_type_info;
static bool missing;

struct SymbolInfo {
    SYMBOL_INFOW info;
    wchar_t name_space[512];
    SymbolInfo()
    {
        ZeroMemory(this, sizeof *this);
        info.SizeOfStruct = sizeof(SYMBOL_INFOW);
        info.MaxNameLen = 512;
    }
};

static uintptr_t address(const wchar_t *symbol)
{
    SymbolInfo s;
    if (sym_from_name(SESSION, symbol, &s.info))
        return (uintptr_t)s.info.Address;
    logf("symbol not found: %ls", symbol);
    missing = true;
    return 0;
}

// A member's offset in a class, searching its base classes too; -1 if absent.
static int member_in(ULONG type, const wchar_t *member)
{
    DWORD count = 0;
    if (!sym_type_info(SESSION, BASE, type, TI_GET_CHILDRENCOUNT, &count) || !count)
        return -1;
    std::string buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG), '\0');
    TI_FINDCHILDREN_PARAMS *kids = (TI_FINDCHILDREN_PARAMS *)&buf[0];
    kids->Count = count;
    if (!sym_type_info(SESSION, BASE, type, TI_FINDCHILDREN, kids))
        return -1;
    for (ULONG i = 0; i < count; i++) {
        ULONG child = kids->ChildId[i];
        DWORD tag = 0, offset = 0;
        sym_type_info(SESSION, BASE, child, TI_GET_SYMTAG, &tag);
        if (tag == TAG_BASECLASS) {
            ULONG base_type = 0;
            sym_type_info(SESSION, BASE, child, TI_GET_TYPE, &base_type);
            sym_type_info(SESSION, BASE, child, TI_GET_OFFSET, &offset);
            int inner = member_in(base_type, member);
            if (inner >= 0)
                return (int)offset + inner;
        } else if (tag == TAG_DATA) {
            WCHAR *child_name = nullptr;
            if (!sym_type_info(SESSION, BASE, child, TI_GET_SYMNAME, &child_name) || !child_name)
                continue;
            bool match = lstrcmpW(child_name, member) == 0;
            LocalFree(child_name);
            if (match && sym_type_info(SESSION, BASE, child, TI_GET_OFFSET, &offset))
                return (int)offset;
        }
    }
    return -1;
}

static ULONG type_of(const wchar_t *cls)
{
    SymbolInfo s;
    return sym_type_from_name(SESSION, BASE, cls, &s.info) ? s.info.TypeIndex : 0;
}

static int field(const wchar_t *cls, const wchar_t *member)
{
    ULONG type = type_of(cls);
    int offset = type ? member_in(type, member) : -1;
    if (offset < 0) {
        logf("field not found: %ls::%ls", cls, member);
        missing = true;
    }
    return offset;
}

static int size_of(const wchar_t *cls)
{
    ULONG64 length = 0;
    ULONG type = type_of(cls);
    if (!type || !sym_type_info(SESSION, BASE, type, TI_GET_LENGTH, &length)) {
        logf("type not found: %ls", cls);
        missing = true;
        return 0;
    }
    return (int)length;
}

bool init(const std::wstring &dir)
{
    DWORD start = GetTickCount();
    HMODULE dbghelp = GetModuleHandleW(L"dbghelp.dll");
    if (!dbghelp)
        dbghelp = LoadLibraryW((dir + L"\\dbghelp.dll").c_str());
    if (!dbghelp) {
        logf("dbghelp.dll not found");
        return false;
    }
    auto sym_init = (SymInitializeW_t)GetProcAddress(dbghelp, "SymInitializeW");
    auto sym_options = (SymSetOptions_t)GetProcAddress(dbghelp, "SymSetOptions");
    auto sym_load = (SymLoadModuleExW_t)GetProcAddress(dbghelp, "SymLoadModuleExW");
    auto sym_cleanup = (SymCleanup_t)GetProcAddress(dbghelp, "SymCleanup");
    sym_from_name = (SymFromNameW_t)GetProcAddress(dbghelp, "SymFromNameW");
    sym_type_from_name = (SymGetTypeFromNameW_t)GetProcAddress(dbghelp, "SymGetTypeFromNameW");
    sym_type_info = (SymGetTypeInfo_t)GetProcAddress(dbghelp, "SymGetTypeInfo");
    if (!sym_init || !sym_options || !sym_load || !sym_cleanup || !sym_from_name || !sym_type_from_name || !sym_type_info) {
        logf("dbghelp.dll lacks the functions we need");
        return false;
    }
    sym_options(SYMOPT_UNDNAME);
    if (!sym_init(SESSION, dir.c_str(), FALSE) ||
        !sym_load(SESSION, nullptr, (dir + L"\\FIELDOFGLORY2.EXE").c_str(), nullptr, BASE, 0, nullptr, 0)) {
        logf("could not load the game's symbols (error %lu)", GetLastError());
        return false;
    }
    missing = false;

    ui_manager_ptr = address(L"gUIManager");
    L.ui_layers = field(L"CUIManager", L"mLayers");
    L.ui_focus = field(L"CUIManager", L"mControlFocus");
    L.layer_size = size_of(L"TUILayer");
    L.screen_root = field(L"TUIScreen", L"screen");
    L.screen_name = field(L"TUIScreen", L"filename");
    L.screen_hidden = field(L"TUIScreen", L"hidden");
    L.screen_flags = field(L"TUIScreen", L"flags");
    L.obj_visible = field(L"CUIObject", L"mVisible");
    L.obj_enabled = field(L"CUIObject", L"mEnabled");
    L.obj_box = field(L"CUIObject", L"mBoundingBox");
    L.obj_name = field(L"CUIObject", L"mName");
    L.obj_tooltip = field(L"CUIObject", L"mToolTipString");
    L.obj_children = field(L"CUIObject", L"mChildren");
    L.obj_sibling = field(L"CUIObject", L"mSibling");
    L.button_string = field(L"CUIButton", L"mString");
    L.button_is_checkbox = field(L"CUIButton", L"mIsCheckbox");
    L.button_checked = field(L"CUIButton", L"mIsChecked");
    L.button_greyed = field(L"CUIButton", L"mGreyed");
    L.button_indented = field(L"CUIButton", L"mIndented");
    L.bar_progress = field(L"CUIProgressBar", L"mProgress");
    L.text_string = field(L"CUIText", L"mString");
    L.list_items = field(L"CUIListBox", L"mItems");
    L.list_selected = field(L"CUIListBox", L"mSelectedItem");
    L.list_dropdown = field(L"CUIListBox", L"mIsDropdown");
    L.item_size = size_of(L"CUIListBoxItem");
    L.item_string = field(L"CUIListBoxItem", L"string");
    L.item_data = field(L"CUIListBoxItem", L"data");
    L.obj_id = field(L"CUIObject", L"mID");
    L.obj_script = field(L"CUIObject", L"mScript");
    fn_get_function = address(L"CCScriptInstance::GetFunction"); // the (name, int *) one: see call_script
    // SymFromName gives the CallFunction(name, args, extra) overload; it looks the name up and calls
    // CallFunction(index, args, extra), its first direct call (E8 rel32): that is the one we want.
    fn_call_function = 0;
    if (const uint8_t *by_name = (const uint8_t *)address(L"CCScriptInstance::CallFunction"))
        for (int i = 0; i < 0x80 && !fn_call_function; i++)
            if (by_name[i] == 0xE8)
                fn_call_function = (uintptr_t)(by_name + i + 5 + *(const int32_t *)(by_name + i + 1));
    fn_item_count = address(L"CUIListBox::GetItemCount");
    fn_set_selected = address(L"CUIListBox::SetSelectedItemIndex");
    fn_message = address(L"CUIObject::Message");
    L.edit_string = field(L"CUIEditBox", L"mString");
    fn_request_focus = address(L"CUIManager::RequestFocus"); // void (CUIObject *), thiscall
    fn_clear_focus = address(L"CUIManager::ClearFocus");     // void ()

    game_ptr = address(L"gGame");
    battle_map_ptr = address(L"gBattleMap");
    side_data_ptr = address(L"gSideData");
    string_store_ptr = address(L"gStringStore");
    terrain_table = address(L"CCombat::mTerrain");
    terrain_count_ptr = address(L"CCombat::mTerrainCount");
    fn_do_pick = address(L"CBattleDisplay::DoPick");
    fn_handle_mouse = address(L"CBattleDisplay::HandleMouse");
    fn_select_unit = address(L"CBattleDisplay::SelectUnit");
    fn_do_action = address(L"CBattleDisplay::DoActionOnCurrentUnit");
    mouse_ptr = address(L"gMouse");
    block_manager_ptr = address(L"gUIBlockManager");
    fn_control_under_mouse = address(L"CUIManager::GetControlUnderMouse"); // the no-argument one
    fn_over_block = address(L"CUIBlockManager::OverUIBlock");
    fn_minimap_pos = address(L"CBattleDisplay::GetMiniMapPos");
    fn_fixed_height = address(L"CBattleMap::GetFixedPointHeight");
    L.sd_attrib_names = field(L"CSideData", L"mAttribValue");
    L.sd_attrib_count = field(L"CSideData", L"mAttribValueCount");
    L.attrib_size = size_of(L"TAttrib");
    L.unit_attrib = field(L"CUnit", L"mAttrib");
    L.template_attrib = field(L"TSquadTemplate", L"mTemplateAttrib");
    fn_find_entry = address(L"CStringStore::FindEntry");
    L.game_battle = field(L"CGame", L"mBattle");
    L.bd_my_side = field(L"CBattleDisplay", L"mMySide");
    L.bd_current_unit = field(L"CBattleDisplay", L"mCurrentUnit");
    L.bd_current_tile = field(L"CBattleDisplay", L"mCurrentTile");
    L.bd_sides = field(L"CBattleDisplay", L"mSides");
    L.bd_options = field(L"CBattleDisplay", L"mOptions");
    L.bd_option_count = field(L"CBattleDisplay", L"mOptionsWalk");
    L.bd_selected_tile = field(L"CBattleDisplay", L"mSelectedTile");
    L.bd_deploy_mode = field(L"CBattleDisplay", L"mDeployMode");
    L.option_size = size_of(L"TOptionEntry");
    L.option_object = field(L"TOptionEntry", L"object");
    L.side_size = size_of(L"CSide");
    L.side_units = field(L"CSide", L"mUnit");
    L.side_unit_count = field(L"CSide", L"mUnitCount");
    L.unit_size = size_of(L"CUnit");
    L.unit_tile = field(L"CUnit", L"mTile");
    L.unit_type = field(L"CUnit", L"mType");
    L.unit_side = field(L"CUnit", L"mSide");
    L.unit_facing = field(L"CUnit", L"mUnitFacing");
    L.unit_dead = field(L"CUnit", L"mDead");
    L.unit_removed = field(L"CUnit", L"mRemoved");
    L.unit_force_visible = field(L"CUnit", L"mForceVisible");
    L.map_tiles = field(L"CBattleMap", L"mTiles");
    L.map_width = field(L"CBattleMap", L"mWidth");
    L.map_height = field(L"CBattleMap", L"mHeight");
    L.tile_size = size_of(L"_TMapTile");
    L.tile_los = field(L"_TMapTile", L"losFlags");
    L.tile_unit = field(L"_TMapTile", L"unit");
    L.tile_terrain = field(L"_TMapTile", L"terrain");
    L.sd_templates = field(L"CSideData", L"mSquadTemplates");
    L.template_id = field(L"TSquadTemplate", L"mID");
    L.template_cost = field(L"TSquadTemplate", L"mCost");
    L.inst_script = field(L"CCScriptInstance", L"mScript");
    L.script_helper = field(L"CCScript", L"mHelper");
    L.helper_var_names = field(L"CScriptHelper", L"mVarNames");
    L.helper_vars = field(L"CScriptHelper", L"mVars");
    L.helper_structs = field(L"CScriptHelper", L"mStruct");
    L.varname_size = size_of(L"__TScriptVariableName");
    L.varname_index = field(L"__TScriptVariableName", L"varIndex");
    L.var_size = size_of(L"__TScriptVariable");
    L.var_data = field(L"__TScriptVariable", L"data");
    L.struct_size = size_of(L"CScriptStruct");
    L.struct_bytes = field(L"CScriptStruct", L"mSize");
    L.struct_name = field(L"CScriptStruct", L"mName");
    L.struct_elements = field(L"CScriptStruct", L"mElements");
    L.element_size = size_of(L"TStructEntry");
    L.element_offset = field(L"TStructEntry", L"offset");
    L.element_name = field(L"TStructEntry", L"name");
    L.terrain_size = size_of(L"TTerrainInfo");

    // Trigger is virtual: find its slot in CUIButton's vtable.
    uintptr_t trigger_fn = address(L"CUIButton::Trigger");
    const uintptr_t *vtable = (const uintptr_t *)address(L"CUIButton::`vftable'");
    trigger_slot = -1;
    for (int i = 0; vtable && trigger_fn && i < 128; i++)
        if (vtable[i] == trigger_fn) {
            trigger_slot = i;
            break;
        }
    if (trigger_slot < 0) {
        logf("CUIButton::Trigger not found in the vtable");
        missing = true;
    }

    sym_cleanup(SESSION);
    logf("engine symbols %s in %lu ms (trigger slot %d)", missing ? "INCOMPLETE" : "loaded", GetTickCount() - start,
         trigger_slot);
    return !missing;
}

// --- RTTI ---
// MSVC: vtable[-1] -> CompleteObjectLocator {sig, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor*};
// hierarchy {sig, attributes, numBaseClasses, BaseClassDescriptor**}; each base {TypeDescriptor*, ...};
// TypeDescriptor {vtable, spare, char name[]} with names like ".?AVCUIButton@@".

unsigned kinds(const void *obj)
{
    static std::unordered_map<uintptr_t, unsigned> cache;
    uintptr_t vtable = *(const uintptr_t *)obj;
    auto hit = cache.find(vtable);
    if (hit != cache.end())
        return hit->second;
    unsigned k = 0;
    const uintptr_t *locator = *(const uintptr_t **)(vtable - 4);
    const uintptr_t *hierarchy = (const uintptr_t *)locator[4];
    unsigned count = (unsigned)hierarchy[2];
    const uintptr_t *bases = (const uintptr_t *)hierarchy[3];
    for (unsigned i = 0; i < count; i++) {
        const char *type_name = (const char *)(*(const uintptr_t *)bases[i] + 8);
        if (!lstrcmpA(type_name, ".?AVCUIButton@@"))
            k |= BUTTON;
        else if (!lstrcmpA(type_name, ".?AVCUIText@@"))
            k |= TEXT;
        else if (!lstrcmpA(type_name, ".?AVCUIListBox@@"))
            k |= LISTBOX;
        else if (!lstrcmpA(type_name, ".?AVCUIEditBox@@"))
            k |= EDITBOX;
        else if (!lstrcmpA(type_name, ".?AVCUIProgressBar@@"))
            k |= BAR;
    }
    cache[vtable] = k;
    return k;
}

std::wstring kstring(const void *base, int offset)
{
    const wchar_t *text = at<const wchar_t *>(base, offset);
    int length = at<int>(base, offset + 8);
    if (!text || length <= 0)
        return {};
    return std::wstring(text, wcsnlen(text, length));
}

// --- the guard ---
// A vectored handler sees the fault first; when it is in this DLL (the mod's code or the C library linked
// into it) on the thread inside guarded(), it puts back the thread's SEH chain head (fs:0) as it was on
// entry and jumps back to guarded() (__builtin_setjmp / __builtin_longjmp: only the frame and stack
// pointers and the resume address, no unwinding). The mod's frames use DWARF unwinding, so no SEH
// records of ours are skipped; game code we called has returned by then (faults there aren't ours).
const char *volatile stage = "";
static void *jump_buf[5];
static volatile bool guard_on;
static DWORD guard_thread, guard_seh, fault_code;
static uintptr_t self_lo, self_hi, fault_at;
static PVOID veh;

static DWORD read_seh()
{
    DWORD v;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(v));
    return v;
}

static void write_seh(DWORD v) { __asm__ volatile("movl %0, %%fs:0" : : "r"(v) : "memory"); }

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    if (!guard_on || GetCurrentThreadId() != guard_thread || at < self_lo || at >= self_hi ||
        (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION))
        return EXCEPTION_CONTINUE_SEARCH;
    guard_on = false;
    fault_code = code;
    fault_at = at - self_lo;
    write_seh(guard_seh);
    __builtin_longjmp(jump_buf, 1);
}

void guard_start()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&on_fault, &self);
    if (!self)
        return;
    const char *base = (const char *)self;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    self_lo = (uintptr_t)base;
    self_hi = self_lo + nt->OptionalHeader.SizeOfImage;
    veh = AddVectoredExceptionHandler(1, on_fault);
}

void guard_stop()
{
    if (veh)
        RemoveVectoredExceptionHandler(veh);
    veh = nullptr;
}

bool guarded(void (*fn)(void *), void *ctx)
{
    if (guard_on || !veh) { // nested, or no handler: just run it
        fn(ctx);
        return true;
    }
    guard_thread = GetCurrentThreadId();
    guard_seh = read_seh();
    if (__builtin_setjmp(jump_buf)) {
        logf("caught a fault in the mod at +0x%x (code %08lx) in %s; carrying on", (unsigned)fault_at, fault_code,
             (const char *)stage);
        return false;
    }
    guard_on = true;
    fn(ctx);
    guard_on = false;
    return true;
}

std::wstring name(const void *obj)
{
    const char *n = (const char *)obj + L.obj_name;
    return std::wstring(n, n + strnlen(n, 32));
}

std::wstring plain(const std::wstring &s)
{
    // Mouse-button pictures stand for the mod's key (user, 2026-10-31): both clicks are Enter there,
    // so "<img = left_mouse.dds>ing on it" reads "pressing Enter on it".
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != L'<') {
            out += s[i] == L'~' ? L'\n' : s[i]; // ~ is a line break in the language files
            i++;
            continue;
        }
        size_t end = s.find(L'>', i);
        if (end == std::wstring::npos)
            break;
        std::wstring tag = s.substr(i, end - i);
        i = end + 1;
        if (tag.find(L"left_mouse") != std::wstring::npos || tag.find(L"right_mouse") != std::wstring::npos) {
            if (s.compare(i, 3, L"ing") == 0) {
                out += L"pressing Enter";
                i += 3;
            } else {
                out += L"Enter";
            }
        }
    }
    return out;
}

std::wstring game_string(const char *id)
{
    // FindEntry rather than GetString: it returns null for a missing id instead of
    // logging it and handing back a placeholder.
    typedef const char *(__thiscall * Find_t)(void *self, const char *id);
    void *store = *(void **)string_store_ptr;
    const char *entry = store ? ((Find_t)fn_find_entry)(store, id) : nullptr;
    const wchar_t *text = entry ? *(const wchar_t *const *)(entry + 0x40) : nullptr; // TStringStoreEntry text
    return text ? plain(text) : std::wstring();
}

void trigger(void *button)
{
    typedef void(__thiscall * Trigger_t)(void *self, int data, int flags);
    Trigger_t fn = (*(Trigger_t **)button)[trigger_slot];
    fn(button, 0, 0); // what a left click passes (CUIButton::HandleMouse: 0 left, 1 right, 2 middle)
}

int list_count(void *list)
{
    typedef int(__thiscall * Count_t)(void *self);
    return ((Count_t)fn_item_count)(list);
}

std::wstring list_item(void *list, int index)
{
    const char *items = at<const char *>(list, L.list_items); // CKArray: mArray first
    std::wstring s = kstring(items + index * L.item_size, L.item_string);
    // Table rows keep their columns apart with tabs.
    std::wstring out;
    for (wchar_t c : s)
        if (c == L'	') {
            if (!out.empty() && out.back() != L' ')
                out += L", ";
        } else {
            out += c;
        }
    while (!out.empty() && (out.back() == L' ' || out.back() == L','))
        out.pop_back();
    return out;
}

unsigned list_item_data(void *list, int index)
{
    const char *items = at<const char *>(list, L.list_items);
    return at<unsigned>(items + index * L.item_size, L.item_data);
}

bool call_script(void *obj, const char *function, const int *args, int count)
{
    // As CUnitList::DrawUnitListItem calls UI_ITEM_RENDER: index = GetFunction(name, &extra) (-1 if absent),
    // int args[12] by parameter, CallFunction(index, args, extra).
    typedef int(__thiscall * Get_t)(void *self, const char *name, int *extra);
    typedef void(__thiscall * Call_t)(void *self, int index, int *args, int extra);
    void *script = at<void *>(obj, L.obj_script);
    if (!script || !fn_get_function || !fn_call_function)
        return false;
    int extra = 0;
    int index = ((Get_t)fn_get_function)(script, function, &extra);
    if (index < 0)
        return false;
    int a[12] = {};
    for (int i = 0; i < count && i < 12; i++)
        a[i] = args[i];
    ((Call_t)fn_call_function)(script, index, a, extra);
    return true;
}

// The engine's arrays (CKArray / CKArrayRaw): T *mArray, uint mArraySize, uint mArrayCount, uint mStepSize.
static char *script_helper(void *obj)
{
    char *inst = obj ? at<char *>(obj, L.obj_script) : nullptr;
    char *script = inst ? at<char *>(inst, L.inst_script) : nullptr;
    return script ? at<char *>(script, L.script_helper) : nullptr;
}

void *script_var(void *obj, const char *name)
{
    char *h = script_helper(obj);
    if (!h)
        return nullptr;
    const char *vars = at<const char *>(h, L.helper_vars);
    unsigned var_count = at<unsigned>(h, L.helper_vars + 8);
    for (int b = 0; b < 64; b++) { // the names sit in 64 hash buckets; reading all is a few hundred compares
        const char *bucket = h + L.helper_var_names + b * 16;
        const char *names = at<const char *>(bucket, 0);
        unsigned n = at<unsigned>(bucket, 8);
        for (unsigned k = 0; names && k < n; k++) {
            const char *e = names + k * L.varname_size; // char name[48] first
            if (strncmp(e, name, 48) == 0) {
                unsigned v = at<unsigned>(e, L.varname_index);
                return v < var_count ? at<void *>(vars + v * L.var_size, L.var_data) : nullptr;
            }
        }
    }
    return nullptr;
}

int script_member(void *obj, const char *type, const char *member, int *type_size)
{
    char *h = script_helper(obj);
    if (!h)
        return -1;
    const char *structs = at<const char *>(h, L.helper_structs);
    unsigned n = at<unsigned>(h, L.helper_structs + 8);
    for (unsigned i = 0; structs && i < n; i++) {
        const char *s = structs + i * L.struct_size;
        if (strncmp(s + L.struct_name, type, 48) != 0)
            continue;
        const char *els = at<const char *>(s, L.struct_elements);
        unsigned m = at<unsigned>(s, L.struct_elements + 8);
        for (unsigned k = 0; els && k < m; k++)
            if (strncmp(els + k * L.element_size + L.element_name, member, 48) == 0) {
                if (type_size)
                    *type_size = at<int>(s, L.struct_bytes);
                return at<int>(els + k * L.element_size, L.element_offset);
            }
        return -1;
    }
    return -1;
}

void list_select(void *list, int index)
{
    typedef void(__thiscall * Set_t)(void *self, int index, bool add);
    typedef void(__thiscall * Message_t)(void *self, unsigned id, int data, int event);
    ((Set_t)fn_set_selected)(list, index, false);
    // What CUIListBox::HandleMouse sends after a left click: (mID, button 0, item index).
    ((Message_t)fn_message)(list, at<unsigned>(list, L.obj_id), 0, index);
}

void focus(void *edit)
{
    typedef void(__thiscall * Focus_t)(void *self, void *obj);
    void *ui = *(void **)ui_manager_ptr;
    if (ui && fn_request_focus)
        ((Focus_t)fn_request_focus)(ui, edit);
}

void clear_focus()
{
    typedef void(__thiscall * Clear_t)(void *self);
    void *ui = *(void **)ui_manager_ptr;
    if (ui && fn_clear_focus)
        ((Clear_t)fn_clear_focus)(ui);
}

} // namespace eng
