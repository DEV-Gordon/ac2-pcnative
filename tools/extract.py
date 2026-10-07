#!/usr/bin/env python3
"""Extracts the executables the recompiler needs from the user's own Ace Combat 2 (USA) disc image.

usage: extract.py "Ace Combat 2 (USA).bin" workdir
  workdir/SLUS_004.04      loader
  workdir/dat/000.bin      main game executable  (ACE2.DAT entry 0)
  workdir/dat/001.bin      intro executable      (ACE2.DAT entry 1)
"""
import hashlib, os, struct, sys

def sector(f, lba):
    f.seek(lba * 2352)
    return f.read(2352)[24:24 + 2048]

def find(f, name):
    pvd = sector(f, 16)
    if pvd[1:6] != b'CD001':
        sys.exit("not a PlayStation disc image (MODE2/2352 .bin expected)")
    root, root_size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    for s in range((root_size + 2047) // 2048):
        d = sector(f, root + s)
        off = 0
        while off < 2048 and d[off]:
            n = d[off + 32]
            fname = d[off + 33:off + 33 + n].split(b';')[0].decode('ascii', 'replace')
            if fname == name:
                return struct.unpack_from('<I', d, off + 2)[0], struct.unpack_from('<I', d, off + 10)[0]
            off += d[off]
    sys.exit(f"{name} not found: is this Ace Combat 2 (USA)?")

def read_file(f, lba, size):
    return b''.join(sector(f, lba + i) for i in range((size + 2047) // 2048))[:size]

def main():
    disc, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, 'dat'), exist_ok=True)
    with open(disc, 'rb') as f:
        lba, size = find(f, 'SLUS_004.04')
        slus = read_file(f, lba, size)
        open(os.path.join(out, 'SLUS_004.04'), 'wb').write(slus)
        lba, size = find(f, 'ACE2.DAT')
        head = read_file(f, lba, 2048)
        count = struct.unpack_from('<I', head, 4)[0]
        for i in (0, 1):
            words, sec = struct.unpack_from('<2I', head, 8 + 8 * i)
            data = read_file(f, lba + sec, (words & 0x0FFFFFFF) * 4)
            open(os.path.join(out, 'dat', f'{i:03d}.bin'), 'wb').write(data)
    print(f"extracted SLUS_004.04 ({len(slus)} bytes) and 2 of {count} ACE2.DAT entries to {out}")
    print("SLUS_004.04 md5:", hashlib.md5(slus).hexdigest())

if __name__ == '__main__':
    main()
