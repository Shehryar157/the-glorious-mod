"""Print the game's live UI: the screens that are up (one per layer, mLayers) and their object trees.

  python tools/walk.py            screens that are up, visible objects only
  python tools/walk.py all        every loaded screen (mScreens), hidden objects too
"""
import sys

import mem
import syms

UI = 0x008F44C8
g = mem.Game()
show_all = len(sys.argv) > 1 and sys.argv[1] == "all"
vt_names = {}
TEXT_AT = {"CUIButton": 0x160, "CUIText": 0x140}


def cls(obj):
    vt = g.u32(obj)
    if vt not in vt_names:
        r = syms.at(vt)
        vt_names[vt] = r[0].split("::")[0] if r else hex(vt)
    return vt_names[vt]


def walk(obj, depth, budget=None):
    budget = budget if budget is not None else [3000]
    while obj and budget[0] > 0:
        budget[0] -= 1
        name = g.cstr(obj + 0x78, 32)
        visible, focus, can_focus, enabled = g.u8(obj + 0x10), g.u8(obj + 0x12), g.u8(obj + 0x15), g.u8(obj + 0x54)
        c = cls(obj)
        if visible or show_all:
            text = g.kstring(obj + TEXT_AT[c]) if c in TEXT_AT else ""
            if c == "CUIListBox":
                text = f"items={g.i32(obj + 0x148 + 4)} sel={g.i32(obj + 0x17C)}"
            x0, y0, x1, y1 = (g.i32(obj + 0x58 + i * 4) for i in range(4))
            flags = ("" if visible else " hidden") + (" FOCUS" if focus else "") + ("" if enabled else " disabled")
            print(f"{'  ' * depth}{name} [{c} type={g.u32(obj + 0x4C)}] ({x0},{y0}-{x1},{y1}){flags} {text!r}"[:220])
            walk(g.u32(obj + 0xF4), depth + 1, budget)
        obj = g.u32(obj + 0xF8)


def main():
    ui = g.u32(UI)
    focus, over = g.u32(ui + 0x3FC4), g.u32(ui + 0x3FC8)
    print("focus:", g.cstr(focus + 0x78) if focus else None, "mouseover:", g.cstr(over + 0x78) if over else None)
    if show_all:
        screens = []
        s = g.u32(ui + 0x2C18)
        while s:
            screens.append((g.u32(s + 0x14C), s))
            s = g.u32(s + 0x154)
    else:
        screens = [(i, g.u32(ui + 0x2C20 + i * 8)) for i in range(24) if g.u32(ui + 0x2C20 + i * 8)]
    for layer, s in screens:
        print(f"== layer {layer}: {g.cstr(s, 64)} flags={g.u32(s + 0x148):#x} hidden={g.u8(s + 0x164)}")
        walk(g.u32(s + 0x144), 1)


if __name__ == "__main__":
    main()
