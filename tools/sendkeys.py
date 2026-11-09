"""Post keys to the launcher's focused control, as if typed, without needing it in the foreground.

  python tools/sendkeys.py tab tab down return
Key names: tab, shift+tab, up, down, left, right, home, end, pgup, pgdn, return, space, escape, or a single letter.
Waits 300 ms between keys, then prints the new lines of FoG2Access.log.
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32")
LOG = r"D:\steam\steamapps\common\Field of Glory II\FoG2Access.log"
KEYS = {"tab": 0x09, "return": 0x0D, "escape": 0x1B, "space": 0x20, "home": 0x24, "end": 0x23, "pgup": 0x21, "pgdn": 0x22,
        "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28}


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD), ("hwndActive", wintypes.HWND),
                ("hwndFocus", wintypes.HWND), ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND), ("rcCaret", wintypes.RECT)]


def target():
    hwnd = user32.FindWindowW(None, "FieldOfGlory2") or user32.FindWindowW(None, "FoG2 Gamemenu")
    if not hwnd:
        sys.exit("launcher or game window not found")
    info = GUITHREADINFO(cbSize=ctypes.sizeof(GUITHREADINFO))
    user32.GetGUIThreadInfo(user32.GetWindowThreadProcessId(hwnd, None), ctypes.byref(info))
    return info.hwndFocus or hwnd


def main():
    start = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    for name in sys.argv[1:]:
        shift = name.startswith("shift+")
        key = name[6:] if shift else name
        vk = KEYS.get(key) or ord(key.upper())
        hwnd = target()
        # Shift is read with GetKeyState, which follows the thread's input
        # state; posting can't set it, so shift combos need a real keypress.
        if shift:
            user32.keybd_event(0x10, 0, 0, 0)
        user32.PostMessageW(hwnd, 0x100, vk, 1)
        user32.PostMessageW(hwnd, 0x101, vk, 0xC0000001)
        time.sleep(0.3)
        if shift:
            user32.keybd_event(0x10, 0, 2, 0)
    with open(LOG, "rb") as f:
        f.seek(start)
        print(f.read().decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
