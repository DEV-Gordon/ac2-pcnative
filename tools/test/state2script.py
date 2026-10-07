# Converts a debug state (slotN.state) into a boot card folder + an --input script that replays it.
# usage: state2script.py slot.state out_script.txt card_dir/card0
import sys, os
st, outscript, carddir = sys.argv[1:4]
d = open(st, 'rb').read()
lines = d.split(b'\n', 5)
vb = int(lines[1].split()[1]); nc = int(lines[4].split()[1])
p = sum(len(l) + 1 for l in lines[:5])
os.makedirs(carddir, exist_ok=True); os.makedirs(carddir + '_2', exist_ok=True)
for _ in range(nc):
    e = d.index(b'\n', p); suf, name, size = d[p:e].decode().split('|'); p = e + 1
    open(os.path.join(carddir + suf, name), 'wb').write(d[p:p + int(size)]); p += int(size)
e = d.index(b'\n', p); n = int(d[p:e].split()[1]); p = e + 1
names = ["select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"]
ch = [(int.from_bytes(d[p+6*k:p+6*k+4], 'little'), int.from_bytes(d[p+6*k+4:p+6*k+6], 'little')) for k in range(n)] + [(vb, 0xFFFF)]
out = []; start = {}; prev = 0xFFFF
for v, bt in ch:
    for x in range(16):
        was = not prev >> x & 1; now = not bt >> x & 1
        if now and not was: start[x] = v
        if was and not now: out.append('%d:%s:%d' % (start[x] - 0, names[x], v - start[x]))
    prev = bt
open(outscript, 'w').write(','.join(out))
print('vblank', vb, 'presses', len(out), 'card files', nc)
