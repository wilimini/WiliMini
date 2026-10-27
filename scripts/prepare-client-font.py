#!/usr/bin/env python3
"""Rasterize WenQuanYi at native PSP sizes; retain all BMP CJK characters."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('font', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / 'client/assets/font'
out.mkdir(parents=True, exist_ok=True)
codepoints = sorted(set(range(32, 127)) | set(range(160, 0x300)) | set(range(0x2000, 0x2070)) |
                    set(range(0x2190, 0x2700)) | set(range(0x3000, 0x3100)) |
                    set(range(0x3400, 0xA000)) | set(range(0xFF00, 0xFFF0)))
records, pixels, shared = [], bytearray(), {}
for size in (12, 14, 16):
    font = ImageFont.truetype(str(a.font), size, layout_engine=ImageFont.Layout.BASIC)
    width, height = size + 2, size + 4
    for cp in codepoints:
        im = Image.new('L', (width, height))
        ImageDraw.Draw(im).text((0, size), chr(cp), font=font, fill=255, anchor='ls')
        alpha = [(v + 8) // 17 for v in im.tobytes()]
        packed = bytes(alpha[i] | ((alpha[i + 1] if i + 1 < len(alpha) else 0) << 4)
                       for i in range(0, len(alpha), 2))
        offset = shared.get(packed)
        if offset is None:
            offset = len(pixels)
            shared[packed] = offset
            pixels.extend(packed)
        records.append((cp, size, round(font.getlength(chr(cp))), width, height, offset))
with (out / 'wqy.bmf').open('wb') as f:
    f.write(b'BMFONT01' + struct.pack('<I', len(records)))
    for rec in records:
        f.write(struct.pack('<IBBBBI', *rec))
    f.write(pixels)
(out / 'MANIFEST.json').write_text(json.dumps({
    'family': 'WenQuanYi Micro Hei', 'source': a.font.name,
    'source_sha256': hashlib.sha256(a.font.read_bytes()).hexdigest(),
    'sizes': [12, 14, 16], 'glyphs': len(records),
    'coverage': 'Latin, symbols, punctuation, kana, BMP CJK and fullwidth',
    'format': '4-bit coverage; sorted index; glyphs loaded through a bounded cache',
    'bytes': (out / 'wqy.bmf').stat().st_size,
}, indent=2) + '\n')
icons = []
for i in range(3, 8):
    im = Image.open(root / f'client/assets/wiliwili/{i:02}.png').convert('RGBA')
    icons.append([((alpha // 17) << 12) | ((r // 17) << 8) | ((g // 17) << 4) | (b // 17)
                  for r, g, b, alpha in im.getdata()])
with (root / 'client/assets/wiliwili/sidebar.hpp').open('w') as f:
    f.write('// Generated from the licensed Wiliwili sidebar PNG assets.\n#pragma once\n')
    f.write('static const unsigned short sidebarPixels[5][484] = {\n')
    for icon in icons:
        f.write('{' + ','.join(map(str, icon)) + '},\n')
    f.write('};\n')
print(out / 'wqy.bmf', (out / 'wqy.bmf').stat().st_size)
