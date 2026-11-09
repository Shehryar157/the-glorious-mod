"""Look things up in the game's own symbol file (STUB_TILE_SHADER.pdb) through Windows' dbghelp.

  python tools/syms.py find <regex>        functions/globals whose name matches, with addresses
  python tools/syms.py addr <name>         one symbol's address
  python tools/syms.py at <hex address>    the symbol containing an address
  python tools/syms.py layout <class>      a class's fields: offset, type, name (base classes included)
  python tools/syms.py types <regex>       type (class/struct/enum) names that match

The exe has no ASLR, so these addresses are the live ones.
"""
import ctypes
import re
import sys
from ctypes import wintypes

GAME = r"D:\steam\steamapps\common\Field of Glory II"
EXE = GAME + r"\FIELDOFGLORY2.EXE"
BASE = 0x400000

dbg = ctypes.WinDLL(r"C:\Windows\System32\dbghelp.dll")
HANDLE = ctypes.c_void_p(0xF0F0)  # any unique value identifies our dbghelp session


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", wintypes.WCHAR * 1024)]


def new_info():
    info = SYMBOL_INFOW()
    info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - 1023 * 2 - 2 + 2  # size without the name buffer, +1 char
    info.SizeOfStruct = 88
    info.MaxNameLen = 1024
    return info


ENUM_CB = ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.POINTER(SYMBOL_INFOW), wintypes.ULONG, ctypes.c_void_p)

dbg.SymSetOptions(0x2 | 0x200)  # UNDNAME | DEFERRED_LOADS... (load on first use)
dbg.SymInitializeW(HANDLE, GAME, False)
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                 ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
if not dbg.SymLoadModuleExW(HANDLE, None, EXE, None, BASE, 0, None, 0):
    sys.exit("could not load the game's symbols")
dbg.SymGetTypeInfo.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wintypes.ULONG, ctypes.c_int, ctypes.c_void_p]

# SymGetTypeInfo requests
TI_GET_SYMTAG, TI_GET_SYMNAME, TI_GET_LENGTH, TI_GET_TYPE, TI_GET_TYPEID, TI_GET_BASETYPE = 0, 1, 2, 3, 4, 5
TI_FINDCHILDREN, TI_GET_OFFSET, TI_GET_CHILDRENCOUNT, TI_GET_COUNT = 7, 10, 13, 12
TAG_UDT, TAG_ENUM, TAG_FUNCTIONTYPE, TAG_POINTER, TAG_ARRAY, TAG_BASETYPE, TAG_TYPEDEF, TAG_BASECLASS = 11, 12, 13, 14, 15, 16, 17, 18
TAG_DATA, TAG_VTABLE = 7, 25
BASETYPES = {1: "void", 2: "char", 3: "wchar_t", 6: "int", 7: "uint", 8: "float", 10: "bool", 13: "long",
             14: "ulong", 31: "char16"}


def info(index, request, ctype=wintypes.DWORD):
    out = ctype()
    return out.value if dbg.SymGetTypeInfo(HANDLE, BASE, index, request, ctypes.byref(out)) else None


def type_name_of(index):
    p = ctypes.c_wchar_p()
    if dbg.SymGetTypeInfo(HANDLE, BASE, index, TI_GET_SYMNAME, ctypes.byref(p)) and p.value:
        name = p.value
        ctypes.windll.kernel32.LocalFree(ctypes.cast(p, ctypes.c_void_p))
        return name
    return None


def describe(index):
    tag = info(index, TI_GET_SYMTAG)
    size = info(index, TI_GET_LENGTH, ctypes.c_uint64) or 0
    if tag == TAG_POINTER:
        return describe(info(index, TI_GET_TYPE)) + "*"
    if tag == TAG_ARRAY:
        return f"{describe(info(index, TI_GET_TYPE))}[{info(index, TI_GET_COUNT)}]"
    if tag == TAG_BASETYPE:
        base = BASETYPES.get(info(index, TI_GET_BASETYPE), "?")
        return {("int", 1): "int8", ("int", 2): "int16", ("int", 8): "int64", ("uint", 1): "uint8",
                ("uint", 2): "uint16", ("uint", 8): "uint64", ("float", 8): "double"}.get((base, size), base)
    if tag == TAG_FUNCTIONTYPE:
        return "fn"
    return type_name_of(index) or f"tag{tag}"


def children(index):
    count = info(index, TI_GET_CHILDRENCOUNT) or 0
    if not count:
        return []

    class FINDCHILDREN(ctypes.Structure):
        _fields_ = [("Count", wintypes.ULONG), ("Start", wintypes.ULONG), ("ChildId", wintypes.ULONG * count)]

    fc = FINDCHILDREN(Count=count)
    dbg.SymGetTypeInfo(HANDLE, BASE, index, TI_FINDCHILDREN, ctypes.byref(fc))
    return list(fc.ChildId)


def type_index(name):
    si = new_info()
    dbg.SymGetTypeFromNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wintypes.LPCWSTR, ctypes.c_void_p]
    if not dbg.SymGetTypeFromNameW(HANDLE, BASE, name, ctypes.byref(si)):
        return None
    return si.TypeIndex


def fields(index, base_offset=0):
    """(offset, type, name) for every data member, base classes flattened."""
    out = []
    for child in children(index):
        tag = info(child, TI_GET_SYMTAG)
        if tag == TAG_BASECLASS:
            off = info(child, TI_GET_OFFSET) or 0
            out.append((base_offset + off, "base", type_name_of(child)))
            out += fields(info(child, TI_GET_TYPE), base_offset + off)
        elif tag == TAG_DATA:
            off = info(child, TI_GET_OFFSET)
            if off is None:
                continue  # static member
            out.append((base_offset + off, describe(info(child, TI_GET_TYPE)), type_name_of(child)))
        elif tag == TAG_VTABLE:
            out.append((base_offset, "vtable", ""))
    return out


def layout(name):
    index = type_index(name)
    if index is None:
        return None
    return info(index, TI_GET_LENGTH, ctypes.c_uint64), fields(index)


def find(pattern, types=False):
    rx = re.compile(pattern)
    found = []

    def cb(p, size, ctx):
        s = p.contents
        if rx.search(s.Name):
            found.append((s.Address, s.Name, s.Tag))
        return True

    callback = ENUM_CB(cb)
    if types:
        dbg.SymEnumTypesW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ENUM_CB, ctypes.c_void_p]
        dbg.SymEnumTypesW(HANDLE, BASE, callback, None)
    else:
        dbg.SymEnumSymbolsW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, wintypes.LPCWSTR, ENUM_CB, ctypes.c_void_p]
        dbg.SymEnumSymbolsW(HANDLE, BASE, "*", callback, None)
    return sorted(set(found))


def addr(name):
    si = new_info()
    dbg.SymFromNameW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p]
    return si.Address if dbg.SymFromNameW(HANDLE, name, ctypes.byref(si)) else None


def at(address):
    si = new_info()
    disp = ctypes.c_uint64()
    dbg.SymFromAddrW.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_void_p]
    if dbg.SymFromAddrW(HANDLE, address, ctypes.byref(disp), ctypes.byref(si)):
        return si.Name, disp.value
    return None


def main():
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == "find":
        for a, name, tag in find(arg):
            print(f"{a:08x} {name}")
    elif cmd == "types":
        for a, name, tag in find(arg, types=True):
            print(name)
    elif cmd == "addr":
        a = addr(arg)
        print(f"{a:08x}" if a else "not found")
    elif cmd == "at":
        print(at(int(arg, 16)))
    elif cmd == "layout":
        r = layout(arg)
        if not r:
            sys.exit("no such type")
        size, fl = r
        print(f"{arg}: {size} bytes")
        for off, t, n in fl:
            print(f"  +{off:04x} {t} {n}")


if __name__ == "__main__":
    main()
