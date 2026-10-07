#!/usr/bin/env python3
"""Disassembles the real BIOS kernel (from a Beetle RAM dump) at B0:5B+offset. Analysis only."""
import struct, sys
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
ram = open(sys.argv[1], 'rb').read()
w = lambda a: struct.unpack_from('<I', ram, a & 0x1fffff)[0]
P = w(0x874 + 0x5b * 4)
off = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 24
a = P + off
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
for i in md.disasm(ram[a & 0x1fffff:(a & 0x1fffff) + 4 * n], a):
    print(f"  {i.address:08x}: {i.mnemonic} {i.op_str}")
