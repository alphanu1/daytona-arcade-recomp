"""ROM-free checks: python3 platform/dreamcast-native/tests/test_prepare_scene.py."""
import importlib.util
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location("scene", Path(__file__).resolve().parents[1] / "tools/prepare_scene.py")
scene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scene)

# Sparse source art must survive splitting with neither holes nor duplicates.
pixels = [bytes(4)] * (512 * 512)
for y in range(96, 144):
    for x in range(112, 152):
        pixels[y * 512 + x] = bytes((x // 8, y // 8, 0, 255))
parts = list(scene.chunks(pixels, False))
assert parts and all(len(set(p[3])) <= 16 for p in parts)
rect = (0, 0, 20, 30, 109, 99, 49, 47)
reconstructed = {}
for part in parts:
    clip = scene.intersection(rect, part)
    if clip is None:
        continue
    x, y, w, h, u, v = clip
    for dy in range(h):
        for dx in range(w):
            point = x + dx, y + dy
            assert point not in reconstructed
            reconstructed[point] = part[3][(v + dy) * part[2] + u + dx]
for y in range(47):
    for x in range(49):
        assert reconstructed.get((20 + x, 30 + y), bytes(4)) == pixels[(99 + y) * 512 + 109 + x]
assert scene.intersection(rect, (0, 0, 8, [])) is None
assert len(list(scene.chunks([bytes(4)] * (512 * 512), False))) == 0
trim_source = [bytes(4)] * 64
trim_source[2 * 8 + 3] = bytes((1, 2, 3, 255))
trim_source[4 * 8 + 6] = bytes((1, 2, 3, 255))
assert scene.trim_alpha((20, 30, 8, 8, 0, 0), 8, trim_source) == (23, 32, 4, 3, 3, 2)
assert scene.trim_alpha((20, 30, 2, 2, 0, 0), 8, trim_source) is None

# Encode an 8x8 checker with a separate bit-interleave implementation; then
# corrupt one packed texel and require the lossless check to catch it.
colors = [bytes((i, i, i, 255)) for i in range(16)]
palette = b"DPAL" + struct.pack("<I", 16) + b"".join(colors)
data = bytearray(64)
expected = []
for y in range(8):
    for x in range(8):
        pen = (x + 3 * y) % 16
        expected.append(colors[pen])
        at = ((y & 1) | (x & 1) << 1 | (y & 2) << 1 | (x & 2) << 2
              | (y & 4) << 2 | (x & 4) << 3)
        data[32 + at // 2] |= pen << ((at & 1) * 4)
scene.check_pal4(data, palette, expected, 8)
data[40] ^= 1
try:
    scene.check_pal4(data, palette, expected, 8)
except ValueError:
    pass
else:
    raise AssertionError("corrupted PAL4 texture accepted")

# Rectangles are consecutive8x8 twiddled squares, in either orientation.
for width,height in ((32,8),(8,32)):
    data=bytearray(32+width*height//2);expected=[]
    for y in range(height):
        for x in range(width):
            pen=(x+3*y)%16;expected.append(colors[pen])
            at=(x//8+y//8)*64+((y&1)|((x&1)<<1)|((y&2)<<1)|((x&2)<<2)|((y&4)<<2)|((x&4)<<3))
            data[32+at//2] |= pen<<((at&1)*4)
    scene.check_pal4(data,palette,expected,width,height)

# Synthetic curve only: validate image size, bounded walk and settled endpoint.
program = bytearray(2 * 1024 * 1024)
struct.pack_into("<36h", program, 0x3ad8c, 64, *([0] * 34), 100)
motion = scene.title_motion(program)
assert len(motion) == 44 and motion[:2] == [-512, -512] and motion[-1] == 0
for bad in (program[:100], bytearray(len(program))):
    try:
        scene.title_motion(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("malformed title program accepted")
struct.pack_into("<h", program, 0x3ad8c, 63)
try:
    scene.title_motion(program)
except ValueError:
    pass
else:
    raise AssertionError("non-settling title curve accepted")
print("PASS: PAL4 splitting/texels, clipped geometry and bounded title extraction")
