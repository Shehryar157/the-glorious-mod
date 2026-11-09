"""Read the running game's memory (32-bit FIELDOFGLORY2.EXE) from Python.

  from mem import Game
  g = Game(); g.u32(addr); g.kstring(addr)
"""
import ctypes
import struct
import subprocess

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def find_pid(exe="FIELDOFGLORY2.EXE"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {exe}", "/FO", "CSV", "/NH"], capture_output=True,
                         text=True).stdout
    best = None  # with several copies running (one stuck at startup), take the one using the most memory
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) > 4 and parts[0].lower() == exe.lower():
            kb = int("".join(ch for ch in parts[4] if ch.isdigit()) or 0)
            if not best or kb > best[0]:
                best = (kb, int(parts[1]))
    if not best:
        raise SystemExit("game not running")
    return best[1]


class Game:
    def __init__(self):
        self.h = k32.OpenProcess(0x0410, False, find_pid())  # QUERY_INFORMATION | VM_READ

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not addr or not k32.ReadProcessMemory(self.h, addr, buf, size, ctypes.byref(n)):
            return None
        return buf.raw

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else 0

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<i", b)[0] if b else 0

    def u8(self, addr):
        b = self.read(addr, 1)
        return b[0] if b else 0

    def cstr(self, addr, size=64):
        b = self.read(addr, size)
        return b.split(b"\0")[0].decode("latin-1") if b else ""

    def wstr(self, addr, chars):
        if not addr or chars <= 0:
            return ""
        b = self.read(addr, min(chars, 4096) * 2)
        return b.decode("utf-16-le", "replace").split("\0")[0] if b else ""

    def kstring(self, addr):
        """CKString: uint16 *mText, int mSize, int mLen."""
        text, size, length = struct.unpack("<IiI", self.read(addr, 12) or b"\0" * 12)
        return self.wstr(text, length if 0 < length < 100000 else 512)
