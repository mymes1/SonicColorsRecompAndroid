# Android image codec fixtures

`runtime/platform/android/image.cpp` has to decode PNGs without SDL3, WIC, libpng or zlib, so it
carries its own inflate. These files exist so a bug in it is caught by comparison against something
that was *not* written here.

Each `<name>.png` is a hand-built PNG (only `IHDR`/`PLTE`/`tRNS`/`IDAT`/`IEND`, CRC via
`zlib.crc32`, compressed with `zlib.compress`) and each `<name>.raw` is the expected RGBA8 output,
rows top to bottom.

| fixture | covers |
| --- | --- |
| `rgba.png` | colour type 6, dynamic Huffman, random pixels |
| `rgb.png` | colour type 2, level-9 compression, filters 0–4 in rotation |
| `pal.png` | colour type 3, `PLTE` + `tRNS` palette alpha |
| `gray.png` | colour type 0 |
| `graya.png` | colour type 4 |
| `big.png` | 300×200, highly compressible: exercises long back-references |

`roundtrip.png` is not checked in: it is written by the test at run time and deleted with the rest
of the build output.

To regenerate, use Python 3 (standard library only):

```python
import zlib, struct, random
# A tiny PNG writer is all that is needed: IHDR(w,h,8,colourType,0,0,0), optional PLTE/tRNS,
# one IDAT of zlib.compress(rows), IEND. Prefix each row with its filter byte and reversibly
# subtract the earlier pixels (filters 0=none, 1=Sub, 2=Up, 3=Average, 4=Paeth).
```

The checked-in copies were produced with `random.seed(7)` and a 37×29 canvas, so any regeneration
that keeps those two facts reproduces them byte for byte.
