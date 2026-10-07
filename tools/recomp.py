#!/usr/bin/env python3
"""
Static recompiler: PlayStation (MIPS R3000A) executables -> C.

Every MIPS function becomes a C function `void F_xxxxxxxx(CPU* c)` that works on the register
context and the emulated 2 MB RAM. Control flow inside a function uses labels/goto, `jal` becomes a
direct C call, indirect calls/jumps go through a lookup table at runtime.

usage: recomp.py config.json outdir
"""
import json
import os
import struct
import sys
from collections import defaultdict

# ----------------------------------------------------------------------------- decoding
REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Ins:
    __slots__ = ('addr', 'w', 'op', 'rs', 'rt', 'rd', 'sa', 'fn', 'imm', 'simm', 'target', 'name')

    def __init__(self, addr, w):
        self.addr = addr
        self.w = w
        self.op = w >> 26
        self.rs = (w >> 21) & 31
        self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31
        self.sa = (w >> 6) & 31
        self.fn = w & 63
        self.imm = w & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        self.name = decode_name(self)

    def branch_target(self):
        return (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF


SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 12: 'syscall',
           13: 'break', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div',
           27: 'divu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor',
           42: 'slt', 43: 'sltu'}
PRIMARY = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu', 10: 'slti',
           11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 16: 'cop0', 18: 'cop2', 32: 'lb', 33: 'lh',
           34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw',
           46: 'swr', 50: 'lwc2', 58: 'swc2'}


def decode_name(i):
    if i.op == 0:
        return SPECIAL.get(i.fn, '?')
    if i.op == 1:
        return {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(i.rt, '?')
    if i.op == 16:
        if i.rs == 0: return 'mfc0'
        if i.rs == 4: return 'mtc0'
        if i.rs == 16 and i.fn == 16: return 'rfe'
        return '?'
    if i.op == 18:
        if i.rs & 16: return 'cop2cmd'
        return {0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}.get(i.rs, '?')
    return PRIMARY.get(i.op, '?')


COND_BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'}
LOADS = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr'}
STORES = {'sb', 'sh', 'sw', 'swl', 'swr'}


def is_prologue(w):
    return (w >> 16) == 0x27BD and (w & 0x8000) != 0     # addiu sp, sp, -N


# ----------------------------------------------------------------------------- program
class Program:
    def __init__(self, cfg):
        self.mods = []
        for m in cfg['modules']:
            raw = open(m['path'], 'rb').read()
            if raw[:8] == b'PS-X EXE':
                entry, gp, base, size = struct.unpack_from('<4I', raw, 0x10)
                body = raw[0x800:0x800 + size]
            else:
                base, body, entry = int(m['base'], 16), raw, None
            self.mods.append({'name': m['name'], 'base': base, 'end': base + len(body), 'body': body,
                              'entry': entry})
        self.extra_seeds = [int(x, 16) for x in cfg.get('seeds', [])]
        # candidate starts that are dropped again if analysis shows they are not functions (code gaps)
        self.weak_seeds = [int(x, 16) for x in cfg.get('weak_seeds', [])]
        self.not_functions = set(int(x, 16) for x in cfg.get('not_functions', []))
        self.func_end_hint = {int(k, 16): int(v, 16) for k, v in cfg.get('function_ends', {}).items()}
        self.hooks = set(int(x, 16) for x in cfg.get('hooks', []))

    def mod_of(self, a):
        for m in self.mods:
            if m['base'] <= a < m['end']:
                return m
        return None

    def word(self, a):
        m = self.mod_of(a)
        if not m:
            return None
        return struct.unpack_from('<I', m['body'], a - m['base'])[0]

    def ins(self, a):
        w = self.word(a)
        return None if w is None else Ins(a, w)


# ----------------------------------------------------------------------------- jump tables
def find_jump_table(prog, fstart, jr_addr):
    """Resolves `jr rX` fed by `lw rX, off(rB)` where rB = table_base + index*4.
    Every `lui` before the load is simulated forward; the first candidate whose entries all point
    into the function is accepted. Returns (table_addr, targets) or None."""
    jr = prog.ins(jr_addr)
    reg = jr.rs
    load = None
    a = jr_addr - 4
    while a >= max(fstart, jr_addr - 64):
        i = prog.ins(a)
        if i.name == 'lw' and i.rt == reg:
            load = i
            break
        if reg in writes_of(i):
            return None
        a -= 4
    if not load:
        return None
    bound = None
    b = jr_addr - 4
    while b >= max(fstart, jr_addr - 128):
        i = prog.ins(b)
        if i.name == 'sltiu':
            bound = i.imm
            break
        b -= 4
    lo = max(fstart, load.addr - 2048)
    cands = []
    p = load.addr - 4
    while p >= lo:
        i = prog.ins(p)
        if i.name == 'lui':
            known = {i.rt: i.imm << 16}
            q = p + 4
            while q < load.addr and known:
                j = prog.ins(q)
                if j.name in ('addiu', 'ori') and j.rs in known:
                    known[j.rt] = known[j.rs] + (j.simm if j.name == 'addiu' else j.imm)
                elif j.name == 'addu' and (j.rs in known) != (j.rt in known):
                    known[j.rd] = known[j.rs] if j.rs in known else known[j.rt]
                else:
                    for w in writes_of(j):
                        known.pop(w, None)
                q += 4
            if load.rs in known:
                cands.append((known[load.rs] + load.simm) & 0xFFFFFFFF)
        p -= 4
    for table in cands:
        if not prog.mod_of(table):
            continue
        targets = []
        n = bound if bound is not None and 0 < bound <= 1024 else 1024
        ok = True
        for k in range(n):
            w = prog.word(table + 4 * k)
            good = w is not None and not (w & 3) and fstart <= w < fstart + 0x20000 and prog.ins(w) is not None \
                and prog.ins(w).name != '?'
            if not good:
                if bound is None and targets:
                    break
                ok = False
                break
            targets.append(w)
        if ok and targets:
            return table, targets
    return None


def writes_of(i):
    """Registers written by an instruction (approximate, enough for jump-table tracking)."""
    n = i.name
    if n in ('addu', 'add', 'subu', 'sub', 'and', 'or', 'xor', 'nor', 'slt', 'sltu', 'sll', 'srl', 'sra',
             'sllv', 'srlv', 'srav', 'mfhi', 'mflo', 'jalr'):
        return {i.rd}
    if n in ('addiu', 'addi', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lui', 'lb', 'lbu', 'lh', 'lhu', 'lw',
             'lwl', 'lwr', 'mfc0', 'mfc2', 'cfc2'):
        return {i.rt}
    if n in ('jal', 'bltzal', 'bgezal'):
        return {31}
    return set()


# ----------------------------------------------------------------------------- function discovery
class Function:
    def __init__(self, start):
        self.start = start
        self.instrs = {}          # addr -> Ins (includes delay slots)
        self.labels = set()
        self.jtables = {}         # jr addr -> list of targets
        self.calls = set()        # jal targets
        self.tails = set()        # j targets that leave the function
        self.bad_exits = set()    # branches to non-function addresses outside the range (boundary is wrong)
        self.end = start


def analyze_function(prog, start, known_starts):
    f = Function(start)
    nexts = [s for s in known_starts if s > start]
    limit = min(nexts) if nexts else prog.mod_of(start)['end']
    if start in prog.func_end_hint:
        limit = prog.func_end_hint[start]
    work = [start]
    seen = set()

    def inside(a):
        return start <= a < limit

    while work:
        a = work.pop()
        while True:
            if a in seen:
                break
            if not inside(a):
                # fell through into the next function: treat as a tail call
                f.tails.add(a)
                break
            i = prog.ins(a)
            if i is None:
                break
            seen.add(a)
            f.instrs[a] = i
            n = i.name
            if n in COND_BRANCHES or n in ('j', 'jal', 'jr', 'jalr'):
                d = prog.ins(a + 4)
                if d is not None and inside(a + 4):
                    seen.add(a + 4)
                    f.instrs[a + 4] = d
            if n in COND_BRANCHES:
                t = i.branch_target()
                if inside(t):
                    f.labels.add(t)
                    work.append(t)
                else:
                    f.tails.add(t)
                    if t not in known_starts and n not in ('bltzal', 'bgezal'):
                        f.bad_exits.add(t)
                if n in ('bltzal', 'bgezal'):
                    f.calls.add(t)
                f.labels.add(a + 8)
                a += 8
                continue
            if n == 'j':
                t = i.target
                if inside(t) and (t not in known_starts or t == start):
                    f.labels.add(t)
                    work.append(t)
                else:
                    f.tails.add(t)
                    if t not in known_starts:
                        f.bad_exits.add(t)
                break
            if n == 'jal':
                f.calls.add(i.target)
                f.labels.add(a + 8)
                a += 8
                continue
            if n == 'jalr':
                f.labels.add(a + 8)
                a += 8
                continue
            if n == 'jr':
                if i.rs != 31:
                    jt = find_jump_table(prog, start, a)
                    if jt:
                        f.jtables[a] = jt[1]
                        for t in jt[1]:
                            if inside(t):
                                f.labels.add(t)
                                work.append(t)
                break
            if n == '?' or n == 'break':
                break
            a += 4
    f.end = max(f.instrs) + 4 if f.instrs else start + 4
    return f


def is_bios_stub(prog, a):
    """`li t2, 0xA0/B0/C0 ; jr t2 ; li t1, n` (possibly reordered)."""
    ws = [prog.ins(a + 4 * k) for k in range(3)]
    if None in ws:
        return None
    vec = num = None
    has_jr = False
    for i in ws:
        if i.name == 'addiu' and i.rs == 0 and i.rt == 10 and i.imm in (0xA0, 0xB0, 0xC0):
            vec = i.imm
        elif i.name == 'addiu' and i.rs == 0 and i.rt == 9:
            num = i.imm
        elif i.name == 'jr' and i.rs == 10:
            has_jr = True
    if vec and num is not None and has_jr:
        return vec, num
    return None


def discover(prog):
    strong = set(m['entry'] for m in prog.mods if m['entry'])
    strong.update(prog.extra_seeds)
    weak = set()
    for m in prog.mods:
        body = m['body']
        n = len(body) // 4
        words = struct.unpack('<%dI' % n, body[:n * 4])
        for k, v in enumerate(words):
            # function pointers stored in data
            if not (v & 3) and prog.mod_of(v):
                w = prog.word(v)
                if w is not None and is_prologue(w):
                    weak.add(v)
            # a prologue right after "jr ra; <delay>" starts a function
            if k >= 2 and words[k - 2] == 0x03E00008 and is_prologue(v):
                weak.add(m['base'] + 4 * k)
            # data pointer to an address right after a "jr ra; <delay>" (leaf callbacks without prologue)
            if not (v & 3) and prog.mod_of(v) and prog.word(v - 8) == 0x03E00008:
                weak.add(v)
            # "lui rX, hi ; addiu rX, rX, lo" building a code address (callbacks passed as arguments)
            if (v >> 26) == 15 and k + 1 < n:
                nx = words[k + 1]
                rt = (v >> 16) & 31
                if (nx >> 26) == 9 and ((nx >> 21) & 31) == rt:
                    lo = nx & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                    t = (((v & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                    if not (t & 3) and prog.mod_of(t) and (prog.word(t - 8) == 0x03E00008 or
                                                         (prog.word(t) is not None and is_prologue(prog.word(t)))):
                        weak.add(t)
    weak.update(prog.weak_seeds)
    weak -= strong
    weak -= prog.not_functions

    for it in range(40):
        starts = strong | weak
        funcs = {}
        new = set()
        jt_targets = set()
        drop = set()
        for s in sorted(starts):
            f = analyze_function(prog, s, starts)
            funcs[s] = f
            for t in f.calls:
                if prog.mod_of(t) and t not in strong and t not in prog.not_functions:
                    new.add(t)
            for t in f.tails:
                if t in weak:
                    continue
                if prog.mod_of(t) and t not in starts and t not in prog.not_functions and t not in f.bad_exits:
                    new.add(t)
            for tl in f.jtables.values():
                jt_targets.update(tl)
            # a branch past the end into a non-function: the weak starts in between are fake
            for t in f.bad_exits:
                if s < t < s + 0x20000:
                    drop.update(w for w in weak if s < w <= t)
        drop |= weak & jt_targets
        drop -= new
        new -= strong
        if not new and not drop:
            return funcs
        strong |= new
        weak -= drop
        weak -= strong
        prog.not_functions |= (drop - strong)
        print(f"  pass {it}: {len(funcs)} functions, +{len(new)} called, -{len(drop)} fake starts", file=sys.stderr)
    print("  WARNING: discovery did not converge", file=sys.stderr)
    return funcs


# ----------------------------------------------------------------------------- C emission
def R(n):
    return '0' if n == 0 else f'c->r[{n}]'


def W(n, expr):
    return '' if n == 0 else f'c->r[{n}] = {expr};'


def emit_simple(i):
    """C for a non-control-flow instruction (also used for delay slots)."""
    n, rs, rt, rd = i.name, i.rs, i.rt, i.rd
    s, t = R(rs), R(rt)
    ea = f'({s} + {i.simm})' if i.simm else s
    if n == 'sll':
        return W(rd, f'{t} << {i.sa}') if i.w else ''   # nop
    if n == 'srl': return W(rd, f'{t} >> {i.sa}')
    if n == 'sra': return W(rd, f'(uint32_t)((int32_t){t} >> {i.sa})')
    if n == 'sllv': return W(rd, f'{t} << ({s} & 31)')
    if n == 'srlv': return W(rd, f'{t} >> ({s} & 31)')
    if n == 'srav': return W(rd, f'(uint32_t)((int32_t){t} >> ({s} & 31))')
    if n in ('add', 'addu'): return W(rd, f'{s} + {t}')
    if n in ('sub', 'subu'): return W(rd, f'{s} - {t}')
    if n == 'and': return W(rd, f'{s} & {t}')
    if n == 'or': return W(rd, f'{s} | {t}')
    if n == 'xor': return W(rd, f'{s} ^ {t}')
    if n == 'nor': return W(rd, f'~({s} | {t})')
    if n == 'slt': return W(rd, f'(int32_t){s} < (int32_t){t}')
    if n == 'sltu': return W(rd, f'{s} < {t}')
    if n in ('addi', 'addiu'): return W(rt, f'{s} + {i.simm}')
    if n == 'slti': return W(rt, f'(int32_t){s} < {i.simm}')
    if n == 'sltiu': return W(rt, f'{s} < (uint32_t){i.simm}')
    if n == 'andi': return W(rt, f'{s} & {i.imm:#x}')
    if n == 'ori': return W(rt, f'{s} | {i.imm:#x}')
    if n == 'xori': return W(rt, f'{s} ^ {i.imm:#x}')
    if n == 'lui': return W(rt, f'{(i.imm << 16):#x}')
    if n == 'mfhi': return W(rd, 'c->hi')
    if n == 'mflo': return W(rd, 'c->lo')
    if n == 'mthi': return f'c->hi = {s};'
    if n == 'mtlo': return f'c->lo = {s};'
    if n == 'mult': return f'{{ int64_t m = (int64_t)(int32_t){s} * (int32_t){t}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}'
    if n == 'multu': return f'{{ uint64_t m = (uint64_t){s} * {t}; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); }}'
    if n == 'div': return f'cpu_div(c, {s}, {t});'
    if n == 'divu': return f'cpu_divu(c, {s}, {t});'
    if n == 'lb': return W(rt, f'(uint32_t)(int8_t)mem_r8({ea})') or f'mem_r8({ea});'
    if n == 'lbu': return W(rt, f'mem_r8({ea})') or f'mem_r8({ea});'
    if n == 'lh': return W(rt, f'(uint32_t)(int16_t)mem_r16({ea})') or f'mem_r16({ea});'
    if n == 'lhu': return W(rt, f'mem_r16({ea})') or f'mem_r16({ea});'
    if n == 'lw': return W(rt, f'mem_r32({ea})') or f'mem_r32({ea});'
    if n == 'lwl': return W(rt, f'mem_lwl({ea}, {t})')
    if n == 'lwr': return W(rt, f'mem_lwr({ea}, {t})')
    if n == 'sb': return f'mem_w8({ea}, {t});'
    if n == 'sh': return f'mem_w16({ea}, {t});'
    if n == 'sw': return f'mem_w32({ea}, {t});'
    if n == 'swl': return f'mem_swl({ea}, {t});'
    if n == 'swr': return f'mem_swr({ea}, {t});'
    if n == 'mfc0': return W(rt, f'cop0_read(c, {rd})')
    if n == 'mtc0': return f'cop0_write(c, {rd}, {t});'
    if n == 'rfe': return 'cop0_rfe(c);'
    if n == 'mfc2': return W(rt, f'gte_read_data(c, {rd})')
    if n == 'cfc2': return W(rt, f'gte_read_ctrl(c, {rd})')
    if n == 'mtc2': return f'gte_write_data(c, {rd}, {t});'
    if n == 'ctc2': return f'gte_write_ctrl(c, {rd}, {t});'
    if n == 'lwc2': return f'gte_write_data(c, {rt}, mem_r32({ea}));'
    if n == 'swc2': return f'mem_w32({ea}, gte_read_data(c, {rt}));'
    if n == 'cop2cmd': return f'gte_command(c, {i.w & 0x1FFFFFF:#x});'
    if n == 'syscall': return f'cpu_syscall(c, {i.addr:#010x});'
    if n == 'break': return f'cpu_break(c, {i.addr:#010x});'
    return f'cpu_unknown(c, {i.addr:#010x}, {i.w:#010x});'


def cond_expr(i):
    s, t = R(i.rs), R(i.rt)
    return {
        'beq': f'{s} == {t}', 'bne': f'{s} != {t}',
        'blez': f'(int32_t){s} <= 0', 'bgtz': f'(int32_t){s} > 0',
        'bltz': f'(int32_t){s} < 0', 'bgez': f'(int32_t){s} >= 0',
        'bltzal': f'(int32_t){s} < 0', 'bgezal': f'(int32_t){s} >= 0',
    }[i.name]


def fname(a):
    return f'F_{a:08X}'


class Emitter:
    def __init__(self, prog, funcs):
        self.prog = prog
        self.funcs = funcs
        self.bios = {}
        for a in funcs:
            st = is_bios_stub(prog, a)
            if st:
                self.bios[a] = st
        # setjmp: "sw ra,0(a0)" + "sw sp,4(a0)" in the first instructions. Every call site's return
        # address becomes a resume point (the BIOS custom exit / longjmp re-enter functions there).
        self.setjmps = set()
        for a in funcs:
            ws = [prog.word(a + 4 * k) for k in range(3)]
            if ws[0] == 0xAC9F0000 and 0xAC9D0004 in ws:
                self.setjmps.add(a)
        self.resume = {}
        for f in funcs.values():
            for a, i in f.instrs.items():
                if i.name == 'jal' and i.target in self.setjmps and a + 8 in f.instrs:
                    self.resume[a + 8] = f.start
                    f.labels.add(a + 8)

    def call(self, target, indent):
        if target in self.funcs:
            return f'{indent}{fname(target)}(c);'
        return f'{indent}call_indirect(c, {target:#010x});'

    def delay(self, f, a):
        d = f.instrs.get(a + 4)
        if d is None:
            return ''
        if d.name in COND_BRANCHES or d.name in ('j', 'jal', 'jr', 'jalr'):
            return f'/* branch in delay slot {d.addr:08x} ignored */'
        return emit_simple(d)

    def function(self, f):
        p = self.prog
        out = [f'void {fname(f.start)}(CPU* c) {{']
        if f.start in self.bios:
            vec, num = self.bios[f.start]
            out.append(f'    bios_call(c, {vec:#x}, {num:#x});')
            out.append('}')
            return '\n'.join(out)
        labels = set(f.labels)
        used = set()
        body = []
        addrs = sorted(f.instrs)
        # cycle cost of each basic block (2 cycles per instruction, a rough R3000 average)
        starts = {f.start} | (labels & set(addrs))
        block_cost = {}
        for st in starts:
            n = 0
            a2 = st
            while a2 in f.instrs:
                n += 1
                i2 = f.instrs[a2]
                if i2.name in COND_BRANCHES or i2.name in ('j', 'jal', 'jr', 'jalr'):
                    n += 1
                    break
                a2 += 4
                if a2 in starts:
                    break
            block_cost[st] = 2 * n
        body.append(f'    FN_ENTER({f.start:#010x});')
        if f.start in p.hooks:
            body.append(f'    if (hle_hook(c, {f.start:#010x})) return;   /* native replacement */')
        if f.start not in labels:
            body.append(f'    CYC({block_cost[f.start]});')
        skip = set()
        for a in addrs:
            i = f.instrs[a]
            prev = f.instrs.get(a - 4)
            # a delay slot that is not a branch target is folded into its branch
            is_delay = prev is not None and (prev.name in COND_BRANCHES or prev.name in ('j', 'jal', 'jr', 'jalr'))
            if a in labels:
                body.append(f'L_{a:08X}:; CYC({block_cost.get(a, 2)});')
            if is_delay and a not in labels:
                continue
            if is_delay and a in labels:
                # also a jump target: entering here runs the slot and continues sequentially
                body.append(f'    {emit_simple(i)}  /* {a:08x} (delay slot, also a target) */')
                if a + 4 not in f.instrs:
                    body.append(f'    {self.call(a + 4, "").strip()} return;')
                continue
            n = i.name
            ind = '    '
            cmt = f'/* {a:08x} */'
            if n in COND_BRANCHES:
                t = i.branch_target()
                link = f' c->r[31] = {a + 8:#010x};' if n.endswith('al') else ''
                body.append(f'{ind}{{ int cnd = {cond_expr(i)};{link} {cmt}')
                if t in f.instrs:
                    back = f' IRQ_CHECK(c, {a:#010x});' if t <= a else ''
                    if n.endswith('al'):
                        body.append(f'{ind}  if (cnd) {{ {self.delay(f, a)} {self.call(t, "").strip()} goto L_{a + 8:08X}; }} }}')
                        labels.add(a + 8)
                    else:
                        body.append(f'{ind}  if (cnd) {{ {self.delay(f, a)}{back} goto L_{t:08X}; }} }}')
                        used.add(t)
                else:
                    body.append(f'{ind}  if (cnd) {{ {self.delay(f, a)} {self.call(t, "").strip()} return; }} }}')
                if a + 4 not in labels:
                    body.append(f'{ind}{self.delay(f, a)}  /* delay slot, not taken */')
                if a + 8 not in f.instrs:
                    body.append(f'{ind}{self.call(a + 8, "").strip()} return;')
                continue
            if n == 'j':
                t = i.target
                if t in f.instrs and t != f.start or (t == f.start and t in labels):
                    back = f' IRQ_CHECK(c, {a:#010x});' if t <= a else ''
                    body.append(f'{ind}{self.delay(f, a)}{back} goto L_{t:08X}; {cmt}')
                    used.add(t)
                else:
                    body.append(f'{ind}{self.delay(f, a)} {cmt}')
                    body.append(f'{ind}IRQ_CHECK(c, {a:#010x}); {self.call(t, "").strip()} return;')
                continue
            if n == 'jal':
                body.append(f'{ind}{self.delay(f, a)} c->r[31] = {a + 8:#010x}; {cmt}')
                body.append(self.call(i.target, ind))
                if a + 8 in f.instrs:
                    body.append(f'{ind}goto L_{a + 8:08X};')
                    labels.add(a + 8)
                else:
                    body.append(f'{ind}return;')
                continue
            if n == 'jalr':
                body.append(f'{ind}{{ uint32_t tgt = {R(i.rs)}; {self.delay(f, a)} {W(i.rd, f"{a + 8:#010x}")} call_indirect(c, tgt); }} {cmt}')
                if a + 8 in f.instrs:
                    body.append(f'{ind}goto L_{a + 8:08X};')
                    labels.add(a + 8)
                else:
                    body.append(f'{ind}return;')
                continue
            if n == 'jr':
                if i.rs == 31:
                    body.append(f'{ind}{self.delay(f, a)} return; {cmt}')
                    continue
                body.append(f'{ind}{{ uint32_t tgt = {R(i.rs)}; {self.delay(f, a)} {cmt}')
                if a in f.jtables:
                    body.append(f'{ind}  IRQ_CHECK(c, {a:#010x});')
                    body.append(f'{ind}  switch (tgt) {{')
                    for t in sorted(set(f.jtables[a])):
                        if t in f.instrs:
                            body.append(f'{ind}    case {t:#010x}: goto L_{t:08X};')
                            labels.add(t)
                    body.append(f'{ind}  }}')
                body.append(f'{ind}  jump_indirect(c, tgt); return; }}')
                continue
            if n == '?':
                body.append(f'{ind}cpu_unknown(c, {a:#010x}, {i.w:#010x}); return;')
                continue
            code = emit_simple(i)
            body.append(f'{ind}{code} {cmt}' if code else f'{ind}{cmt}')
            # falling off the end of the analyzed region (no explicit return): continue in the next function
            if a + 4 not in f.instrs and n not in ('break',):
                body.append(f'{ind}{self.call(a + 4, "").strip()} return;')
        # emit, dropping labels nobody jumps to (avoids -Wunused-label noise)
        text = '\n'.join(body)
        out.append(text)
        out.append('}')
        return '\n'.join(out)


# ----------------------------------------------------------------------------- main
def main():
    cfg = json.load(open(sys.argv[1]))
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    prog = Program(cfg)
    funcs = discover(prog)
    em = Emitter(prog, funcs)
    print(f"{len(funcs)} functions, {len(em.bios)} BIOS stubs, "
          f"{sum(len(f.jtables) for f in funcs.values())} jump tables", file=sys.stderr)

    print(f"{len(em.setjmps)} setjmp, {len(em.resume)} resume points", file=sys.stderr)
    order = sorted(funcs)
    per_file = 400
    files = []
    for k in range(0, len(order), per_file):
        name = f'recomp_{k // per_file:03d}.c'
        files.append(name)
        with open(os.path.join(outdir, name), 'w') as o:
            o.write('/* generated by tools/recomp.py - do not edit */\n#include "recomp.h"\n\n')
            for a in order[k:k + per_file]:
                o.write(em.function(funcs[a]))
                o.write('\n\n')
                # resume points inside this function: same body, entered at the label
                for r, fs in sorted(em.resume.items()):
                    if fs == a:
                        body = em.function(funcs[a])
                        body = body.replace(f'void {fname(a)}(CPU* c) {{', f'void R_{r:08X}(CPU* c) {{\n    goto L_{r:08X};', 1)
                        o.write(body + '\n\n')
    with open(os.path.join(outdir, 'recomp_funcs.h'), 'w') as o:
        o.write('/* generated */\n#pragma once\n#include "cpu.h"\n')
        for a in order:
            o.write(f'void {fname(a)}(CPU* c);\n')
        for r in sorted(em.resume):
            o.write(f'void R_{r:08X}(CPU* c);\n')
    with open(os.path.join(outdir, 'recomp_table.c'), 'w') as o:
        o.write('/* generated */\n#include "recomp.h"\n\nconst RecompFunc recomp_table[] = {\n')
        entries = [(a, fname(a)) for a in order] + [(r, f'R_{r:08X}') for r in em.resume]
        for a, n in sorted(entries):
            o.write(f'    {{{a:#010x}, {n}}},\n')
        o.write('};\nconst unsigned recomp_table_size = sizeof(recomp_table) / sizeof(recomp_table[0]);\n')
    with open(os.path.join(outdir, 'functions.txt'), 'w') as o:
        for a in order:
            f = funcs[a]
            o.write(f'{a:08x} {f.end:08x} {len(f.instrs):5d} {em.prog.mod_of(a)["name"]}'
                    f'{" BIOS%02X:%02X" % em.bios[a] if a in em.bios else ""}\n')
    with open(os.path.join(outdir, 'sources.cmake'), 'w') as o:
        o.write('set(RECOMP_SOURCES\n' + ''.join(f'  ${{CMAKE_CURRENT_LIST_DIR}}/{n}\n' for n in files + ['recomp_table.c']) + ')\n')


if __name__ == '__main__':
    main()
