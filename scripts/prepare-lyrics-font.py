#!/usr/bin/env python3
"""Rasterize the Source Han Sans CN Heavy subset used by the PSP music screen."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont


def encoded_characters(encoding, leads, trails):
    return {ord(char) for lead in leads for trail in trails
            for char in bytes((lead, trail)).decode(encoding, errors='ignore') if ord(char) > 127}


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('font', type=Path)
args = parser.parse_args()
output = Path(__file__).resolve().parents[1] / 'client/assets/font'
size = 24
font = ImageFont.truetype(str(args.font), size, layout_engine=ImageFont.Layout.BASIC)
if font.getname() != ('Source Han Sans CN', 'Heavy'):
    parser.error('Expected Source Han Sans CN Heavy')

codepoints = set(range(32, 0x250)) | set(range(0x370, 0x530)) | set(range(0x2000, 0x2070))
codepoints |= set(range(0x3000, 0x3130)) | set(range(0xFF00, 0xFFF0))
codepoints |= encoded_characters('gb2312', range(0xB0, 0xF8), range(0xA1, 0xFF))
codepoints |= encoded_characters('big5', range(0xA4, 0xC7), [*range(0x40, 0x7F), *range(0xA1, 0xFF)])
with TTFont(args.font, lazy=True) as source:
    codepoints &= source.getBestCmap().keys()

records, pixels = bytearray(), bytearray()
for cp in sorted(codepoints):
    char = chr(cp)
    _, _, right, bottom = font.getbbox(char, anchor='ls')
    advance = max(1, round(font.getlength(char)))
    # A shared height for most glyphs lets the PSP index merge adjacent codepoints.
    width, height = max(advance, right), size + max(4, bottom)
    if not (0 < width <= 28 and 0 < height <= 32):
        continue  # Let the body font handle unusually large glyphs.
    image = Image.new('L', (width, height))
    ImageDraw.Draw(image).text((0, size), char, font=font, fill=255, anchor='ls')
    alpha = [(value + 8) // 17 for value in image.tobytes()]
    alpha.append(0)
    records.extend(struct.pack('<IBBBBI', cp, size, advance, width, height, len(pixels)))
    pixels.extend(alpha[i] | (alpha[i + 1] << 4) for i in range(0, width * height, 2))

count = len(records) // 12
asset = output / 'lyrics.bmf'
asset.write_bytes(b'BMFONT01' + struct.pack('<I', count) + records + pixels)
(output / 'LYRICS-MANIFEST.json').write_text(json.dumps({
    'family': 'Source Han Sans CN Heavy',
    'source': args.font.name,
    'source_sha256': hashlib.sha256(args.font.read_bytes()).hexdigest(),
    'size': size,
    'glyphs': count,
    'coverage': 'GB2312 Hanzi, Big5 level 1, kana, Latin, Greek, Cyrillic and punctuation',
    'bytes': asset.stat().st_size,
}, indent=2) + '\n')
print(asset, asset.stat().st_size)
