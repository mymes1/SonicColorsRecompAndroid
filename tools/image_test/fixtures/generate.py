#!/usr/bin/env python3
# Regenerates the fixtures in this directory (the PNG writer for tools/image_test).
# Standard library only; the seed and canvas size are what make the output byte-identical.
#
# Regenerate the whole set at once, never one file: every image draws from the same random
# sequence, so `pal.png` from one run and `pal.raw` from another describe different pixels.
import os
import zlib, struct, random

os.chdir(os.path.dirname(os.path.abspath(__file__)))

def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def make(path, w, h, ct, chans, mkrow, level=6, filters=(0, 1, 2, 3, 4), plte=None, trns=None):
    raw = b''
    prev = bytes(w * chans)
    for y in range(h):
        line = mkrow(y)
        f = filters[y % len(filters)]
        out = bytearray()
        for i in range(len(line)):
            a = line[i - chans] if i >= chans else 0
            b = prev[i]
            c = prev[i - chans] if i >= chans else 0
            x = line[i]
            if f == 0: v = x
            elif f == 1: v = x - a
            elif f == 2: v = x - b
            elif f == 3: v = x - ((a + b) >> 1)
            else: v = x - paeth(a, b, c)
            out.append(v & 0xff)
        raw += bytes([f]) + bytes(out)
        prev = line
    ihdr = struct.pack('>IIBBBBB', w, h, 8, ct, 0, 0, 0)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
    if plte: png += chunk(b'PLTE', plte)
    if trns: png += chunk(b'tRNS', trns)
    png += chunk(b'IDAT', zlib.compress(raw, level)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

random.seed(7)
w, h = 37, 29

rgba = [[random.randrange(256) for _ in range(4 * w)] for _ in range(h)]
make('rgba.png', w, h, 6, 4, lambda y: bytes(rgba[y]))
open('rgba.raw', 'wb').write(b''.join(bytes(r) for r in rgba))

rgb = [[random.randrange(256) for _ in range(3 * w)] for _ in range(h)]
make('rgb.png', w, h, 2, 3, lambda y: bytes(rgb[y]), level=9)
exp = bytearray()
for r in rgb:
    for x in range(w):
        exp += bytes(r[3 * x:3 * x + 3]) + b'\xff'
open('rgb.raw', 'wb').write(bytes(exp))

pal = bytes([random.randrange(256) for _ in range(3 * 13)])
trns = bytes([random.randrange(256) for _ in range(13)])
idx = [[random.randrange(13) for _ in range(w)] for _ in range(h)]
make('pal.png', w, h, 3, 1, lambda y: bytes(idx[y]), plte=pal, trns=trns)
exp = bytearray()
for r in idx:
    for v in r:
        exp += pal[v * 3:v * 3 + 3] + bytes([trns[v]])
open('pal.raw', 'wb').write(bytes(exp))

g = [[random.randrange(256) for _ in range(w)] for _ in range(h)]
make('gray.png', w, h, 0, 1, lambda y: bytes(g[y]))
open('gray.raw', 'wb').write(b''.join(b''.join(bytes([v, v, v, 255]) for v in r) for r in g))

ga = [[random.randrange(256) for _ in range(2 * w)] for _ in range(h)]
make('graya.png', w, h, 4, 2, lambda y: bytes(ga[y]))
open('graya.raw', 'wb').write(b''.join(b''.join(bytes([r[2 * x], r[2 * x], r[2 * x], r[2 * x + 1]]) for x in range(w)) for r in ga))

# A large, highly-compressible image forces long match copies (deflate level 9).
w2, h2 = 300, 200
make('big.png', w2, h2, 6, 4, lambda y: bytes([(x // 7 + y) & 0xff for x in range(w2 * 4)]), level=9)
exp = bytearray()
for y in range(h2):
    exp += bytes([(x // 7 + y) & 0xff for x in range(w2 * 4)])
open('big.raw', 'wb').write(bytes(exp))
print('generated')
