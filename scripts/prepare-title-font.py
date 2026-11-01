#!/usr/bin/env python3
"""Rasterize large (32 px) glyphs for the page titles of the tile home styles.

Only ASCII and the characters of the title strings (resources/i18n tile_* labels) are included,
so the file stays small. Same record layout as wqy.bmf: 4-bit coverage, sorted index.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

TITLE_KEYS = ('tile_title', 'tile_recommend', 'tile_hots', 'tile_search', 'tile_discover',
              'tile_dynamic', 'tile_inbox', 'tile_mine', 'tile_settings', 'comments_title',
              'comments_replies_title')
SIZE = 32
GAMMA = 1.25  # lowers coverage a little so the strokes read as light, not bold

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('font', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / 'client/assets/font'
out.mkdir(parents=True, exist_ok=True)

codepoints = set(range(32, 127))
for locale in ('zh-Hans', 'en-US'):
    catalog = json.loads((root / f'resources/i18n/{locale}.json').read_text(encoding='utf-8'))
    for key in TITLE_KEYS:
        codepoints.update(ord(c) for c in catalog[key])
font = ImageFont.truetype(str(a.font), SIZE, layout_engine=ImageFont.Layout.BASIC)
width, height = SIZE + 2, SIZE + 10  # room for descenders at this size
records, pixels = [], bytearray()
for cp in sorted(codepoints):
    im = Image.new('L', (width, height))
    ImageDraw.Draw(im).text((0, SIZE), chr(cp), font=font, fill=255, anchor='ls')
    alpha = [min(15, round(15 * (v / 255) ** GAMMA)) for v in im.tobytes()]
    packed = bytes(alpha[i] | ((alpha[i + 1] if i + 1 < len(alpha) else 0) << 4)
                   for i in range(0, len(alpha), 2))
    records.append((cp, SIZE, round(font.getlength(chr(cp))), width, height, len(pixels)))
    pixels.extend(packed)
with (out / 'title.bmf').open('wb') as f:
    f.write(b'BMFONT01' + struct.pack('<I', len(records)))
    for rec in records:
        f.write(struct.pack('<IBBBBI', *rec))
    f.write(pixels)
(out / 'TITLE-MANIFEST.json').write_text(json.dumps({
    'family': 'WenQuanYi Micro Hei', 'source': a.font.name,
    'source_sha256': hashlib.sha256(a.font.read_bytes()).hexdigest(),
    'size': SIZE, 'glyphs': len(records), 'gamma': GAMMA,
    'coverage': 'ASCII and the characters of the tile_* title strings',
    'bytes': (out / 'title.bmf').stat().st_size,
}, indent=2) + '\n')
print(out / 'title.bmf', (out / 'title.bmf').stat().st_size)
