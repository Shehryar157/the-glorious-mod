"""Disassemble game code from the running game's memory, with symbol names for call targets.
(The exe on disk is encrypted by Steam's DRM wrapper; it decrypts itself in memory.)

  python tools/disasm.py <symbol or hex address> [instruction count]
"""
import sys

import capstone
import mem
import syms

game = mem.Game()


def disasm(addr, count=60):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = game.read(addr, count * 15)
    for n, ins in enumerate(md.disasm(code, addr)):
        if n >= count:
            break
        note = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            r = syms.at(int(ins.op_str, 16))
            note = f"   ; {r[0]}" if r else ""
        print(f"{ins.address:08x} {ins.mnemonic} {ins.op_str}{note}")
        if ins.mnemonic == "ret" or ins.mnemonic == "int3":
            break


if __name__ == "__main__":
    target = sys.argv[1]
    addr = int(target, 16) if all(c in "0123456789abcdefABCDEFx" for c in target) else syms.addr(target)
    disasm(addr, int(sys.argv[2]) if len(sys.argv) > 2 else 60)
