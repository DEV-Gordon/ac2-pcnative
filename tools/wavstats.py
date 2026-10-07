#!/usr/bin/env python3
"""Prints the RMS level of a 44.1 kHz stereo 16-bit WAV, one value per second (dBFS)."""
import math, struct, sys
d = open(sys.argv[1], 'rb').read()[44:]
n = len(d) // 4
per = 44100
for s in range(0, n // per):
    chunk = struct.unpack_from('<%dh' % (per * 2), d, s * per * 4)
    rms = math.sqrt(sum(x * x for x in chunk) / len(chunk))
    db = 20 * math.log10(rms / 32768) if rms > 0 else -120
    print(f"{s:4d}s {db:7.1f} dBFS {'#' * max(0, int((db + 60) / 2))}")
