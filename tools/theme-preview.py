#!/usr/bin/env python3
"""Draws what a theme's layout function returns, so a layout can be checked without a PSP or emulator.

usage: theme-preview.py THEME_DIR SCREEN COUNT OUT.png [--selected N] [--style 0|1|2] [--scroll PX]

Builds the host-side layout runner (needs a C++ compiler and Lua 5.4 source, see tools/test-theme.sh),
runs the theme's layout() for the screen and paints the item rectangles on a 480x272 canvas, the
focused item last. Item numbers are 1-based, like ctx.selected in the script.
"""
import argparse, json, os, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('theme'); parser.add_argument('screen'); parser.add_argument('count', type=int); parser.add_argument('out')
parser.add_argument('--selected', type=int, default=0); parser.add_argument('--style', type=int, default=1)
parser.add_argument('--scroll', type=int, default=None)
args = parser.parse_args()

build = root / 'build/theme-test'
lua = Path(os.environ.get('LUA_SRC', root / 'build/downloads/lua-5.4.7'))
if not (build / 'liblua.a').exists():
    subprocess.run(['sh', str(root / 'tools/test-theme.sh')], check=True, stdout=subprocess.DEVNULL)
dump = build / 'theme-dump'
sources = [root / 'tools/theme_dump.cpp', root / 'client/theme.cpp']
if not dump.exists() or any(dump.stat().st_mtime < s.stat().st_mtime for s in sources):
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-I', str(root / 'client'), '-I', str(lua),
                    *map(str, sources), str(build / 'liblua.a'), '-o', str(dump), '-lm'], check=True)
result = subprocess.run([str(dump), args.theme, args.screen, str(args.count), str(args.selected), str(args.style)],
                        capture_output=True, text=True)
if result.returncode:
    sys.exit(result.stderr.strip() or 'theme-dump failed')
data = json.loads(result.stdout)
if not data['themed']:
    sys.exit('the theme leaves this screen to the built-in layout')
items, horizontal = data['items'], data['horizontal']
anchor = data['anchor'] if data['anchor'] >= 0 else (12 if horizontal else 100)
scroll = args.scroll
if scroll is None and items:
    pick = items[min(args.selected, len(items) - 1)]
    scroll = max(0, (pick[0] if horizontal else pick[1]) - anchor)
scroll = scroll or 0
image = Image.new('RGB', (480, 272), (242, 243, 245))
draw = ImageDraw.Draw(image)
draw.rectangle((0, 258, 480, 272), fill=(220, 222, 226))
order = [i for i in range(len(items)) if i != args.selected] + ([args.selected] if args.selected < len(items) else [])
for i in order:
    x, y, w, h = items[i]
    x -= scroll if horizontal else 0
    y -= 0 if horizontal else scroll
    focused = i == args.selected
    if focused:
        f = data['focus']
        sw, sh = w * f['scale'] // 100, h * f['scale'] // 100
        x += (w - sw) // 2 + f['dx']; y += (h - sh) // 2 + f['dy']; w, h = sw, sh
    draw.rectangle((x, y, x + w - 1, y + h - 1), fill=(90, 140, 220) if focused else (150, 170, 200),
                   outline=(20, 20, 30) if focused else (90, 100, 120))
    draw.text((x + 4, y + 2), str(i + 1), fill=(255, 255, 255))
image.save(args.out)
print(f'{len(items)} items, scroll {scroll} -> {args.out}')
