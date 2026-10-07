#!/usr/bin/env python3
"""Disassembles a PS-X EXE (or a raw blob at a given base) with capstone. Usage: disasm.py file [base] [start] [end]"""
import sys, struct
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN

def load(path, base=None):
    d = open(path, 'rb').read()
    if d[:8] == b'PS-X EXE':
        pc0, gp0, t_addr, t_size = struct.unpack_from('<4I', d, 0x10)
        return t_addr, d[0x800:0x800 + t_size], pc0
    return base, d, None

if __name__ == '__main__':
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    addr, code, pc0 = load(sys.argv[1], base)
    start = int(sys.argv[3], 16) if len(sys.argv) > 3 else addr
    end = int(sys.argv[4], 16) if len(sys.argv) > 4 else addr + len(code)
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    for off in range(start - addr, end - addr, 4):
        w = code[off:off + 4]
        ins = list(md.disasm(w, addr + off))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".word {struct.unpack('<I', w)[0]:#010x}"
        print(f"{addr + off:08x}: {struct.unpack('<I', w)[0]:08x}  {txt}")
