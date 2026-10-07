#!/usr/bin/env python3
"""Prototype STR (BS v2/v3) bitstream decoder, validated against the game's own MDEC input."""
import struct, sys

VLC = [("11",0,1),("011",1,1),("0100",0,2),("0101",2,1),("00101",0,3),("00111",3,1),("00110",4,1),
("000110",1,2),("000111",5,1),("000101",6,1),("000100",7,1),("0000110",0,4),("0000100",2,2),("0000111",8,1),
("0000101",9,1),("00100110",0,5),("00100001",0,6),("00100101",1,3),("00100100",3,2),("00100111",10,1),
("00100011",11,1),("00100010",12,1),("00100000",13,1),("0000001010",0,7),("0000001100",1,4),("0000001011",2,3),
("0000001111",4,2),("0000001001",5,2),("0000001110",14,1),("0000001101",15,1),("0000001000",16,1),
("000000011101",0,8),("000000011000",0,9),("000000010011",0,10),("000000010000",0,11),("000000011011",1,5),
("000000010100",2,4),("000000011100",3,3),("000000010010",4,3),("000000011110",6,2),("000000010101",7,2),
("000000010001",8,2),("000000011111",17,1),("000000011010",18,1),("000000011001",19,1),("000000010111",20,1),
("000000010110",21,1),("0000000011010",0,12),("0000000011001",0,13),("0000000011000",0,14),("0000000010111",0,15),
("0000000010110",1,6),("0000000010101",1,7),("0000000010100",2,5),("0000000010011",3,4),("0000000010010",5,3),
("0000000010001",9,2),("0000000010000",10,2),("0000000011111",22,1),("0000000011110",23,1),("0000000011101",24,1),
("0000000011100",25,1),("0000000011011",26,1),("00000000011111",0,16),("00000000011110",0,17),("00000000011101",0,18),
("00000000011100",0,19),("00000000011011",0,20),("00000000011010",0,21),("00000000011001",0,22),("00000000011000",0,23),
("00000000010111",0,24),("00000000010110",0,25),("00000000010101",0,26),("00000000010100",0,27),("00000000010011",0,28),
("00000000010010",0,29),("00000000010001",0,30),("00000000010000",0,31),("000000000011000",0,32),("000000000010111",0,33),
("000000000010110",0,34),("000000000010101",0,35),("000000000010100",0,36),("000000000010011",0,37),("000000000010010",0,38),
("000000000010001",0,39),("000000000010000",0,40),("000000000011111",1,8),("000000000011110",1,9),("000000000011101",1,10),
("000000000011100",1,11),("000000000011011",1,12),("000000000011010",1,13),("000000000011001",1,14),("0000000000010011",1,15),
("0000000000010010",1,16),("0000000000010001",1,17),("0000000000010000",1,18),("0000000000010100",6,3),("0000000000011010",11,2),
("0000000000011001",12,2),("0000000000011000",13,2),("0000000000010111",14,2),("0000000000010110",15,2),("0000000000010101",16,2),
("0000000000011111",27,1),("0000000000011110",28,1),("0000000000011101",29,1),("0000000000011100",30,1),("0000000000011011",31,1)]
TAB = {c:(r,l) for c,r,l in VLC}
DC_LUMA = {"100":0,"00":1,"01":2,"101":3,"110":4,"1110":5,"11110":6,"111110":7,"1111110":8}
DC_CHROMA = {"00":0,"01":1,"10":2,"110":3,"1110":4,"11110":5,"111110":6,"1111110":7,"11111110":8}

class Bits:
    def __init__(s, data):
        s.bits = ''.join(format(struct.unpack_from('<H', data, i)[0], '016b') for i in range(0, len(data) - 1, 2))
        s.p = 0
    def peek(s, n): return s.bits[s.p:s.p+n]
    def get(s, n):
        v = s.bits[s.p:s.p+n]; s.p += n; return int(v, 2) if v else 0

def read_vlc(b, table, maxlen=16):
    for n in range(1, maxlen + 1):
        c = b.peek(n)
        if c in table:
            b.p += n
            return table[c]
    raise ValueError("bad vlc at %d: %s" % (b.p, b.peek(20)))

def decode(frame, mbs, dcmode):
    q = struct.unpack_from('<H', frame, 4)[0]; ver = struct.unpack_from('<H', frame, 6)[0]
    b = Bits(frame[8:]); out = []
    prev = {'y': 0, 'cr': 0, 'cb': 0}
    for mb in range(mbs):
        for blk, comp in enumerate(('cr', 'cb', 'y', 'y', 'y', 'y')):
            if ver == 2:
                dc = b.get(10); dc = dc - 1024 if dc & 512 else dc
            else:
                size = read_vlc(b, DC_LUMA if comp == 'y' else DC_CHROMA, 8)
                diff = 0
                if size:
                    v = b.get(size)
                    diff = v if v >> (size - 1) else v - (1 << size) + 1
                if dcmode == 'x4': prev[comp] += diff * 4; dc = prev[comp]
                elif dcmode == 'plain': prev[comp] += diff; dc = prev[comp]
                else: prev[comp] += diff; dc = prev[comp] * 4
            out.append((q << 10) | (dc & 0x3FF))
            while True:
                if b.peek(2) == '10': b.p += 2; out.append(0xFE00); break
                if b.peek(6) == '000001':
                    b.p += 6; run = b.get(6); lvl = b.get(10); out.append((run << 10) | (lvl & 0x3FF)); continue
                run, lvl = read_vlc(b, TAB)
                if b.get(1): lvl = -lvl
                out.append((run << 10) | (lvl & 0x3FF))
    return out, ver

def frames(binpath, lba, count):
    f = open(binpath, 'rb'); cur = None; parts = {}
    for s in range(count):
        f.seek((lba + s) * 2352); raw = f.read(2352); d = raw[24:]
        if raw[18] & 4 or d[0:2] != b'\x60\x01': continue
        idx, cnt = struct.unpack_from('<HH', d, 4); fno = struct.unpack_from('<I', d, 8)[0]
        w, h = struct.unpack_from('<HH', d, 16)
        if fno != cur: cur = fno; parts = {}
        parts[idx] = d[32:32 + 2016]
        if len(parts) == cnt:
            yield fno, w, h, b''.join(parts[i] for i in range(cnt))

if __name__ == '__main__':
    game = open(sys.argv[1], 'rb').read()
    g = list(struct.unpack('<%dH' % ((len(game) - 4) // 2), game[4:]))
    print("game first codes:", [hex(x) for x in g[:12]])
    for fno, w, h, data in frames('/home/czarate/Documents/Ac2Decomp/Ace Combat 2 (USA)/Ace Combat 2 (USA).bin', 190188, 40):
        mbs = ((w + 15) // 16) * ((h + 15) // 16)
        for mode in ('x4', 'plain', 'prevx4'):
            try:
                out, ver = decode(data, mbs, mode)
            except Exception as e:
                print(fno, mode, 'error', e); continue
            n = min(len(out), len(g))
            same = sum(1 for i in range(n) if out[i] == g[i])
            print(f"frame {fno} {w}x{h} v{ver} mode={mode}: codes {len(out)} vs game {len(g)}; identical {same}/{n}")
        break
