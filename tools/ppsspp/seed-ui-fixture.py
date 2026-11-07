#!/usr/bin/env python3
"""Seed an isolated headless memstick with offline UI fixtures.

Writes a cached home feed, generated cover images, local history and watch-later
entries, and optionally settings.json, so UI screens can be inspected in
PPSSPP without an account or network access. The data is synthetic.

Usage: seed-ui-fixture.py MEMSTICK_DIR [--tiles | --metro] [--dark] [--many] [--theme NAME]

--many writes 30 history entries so long lists and restored scroll positions can be checked.
"""
import colorsys
import json
import shutil
import sys
from pathlib import Path

from PIL import Image, ImageDraw

TITLES = [
    ("Handheld games you forgot existed", "Old Game Hall", 612),
    ("Building a Bilibili client for 32 MB of RAM", "TinyCircuits Lab", 1380),
    ("Lo-fi study stream, 24h", "Synth Garage", 3600),
    ("Why 480x272 still looks great", "Dot Painter", 421),
    ("Speedrun: the first 10 minutes", "Late Night Runs", 604),
    ("Chiptune night mix", "Synth Garage", 2330),
    ("Pixel art in 16 colours", "Dot Painter", 905),
    ("Cassette radio, restored", "Retro Bench", 1187),
]


def cover(path, hue):
    image = Image.new("RGB", (320, 180))
    draw = ImageDraw.Draw(image)
    for y in range(180):
        r, g, b = colorsys.hsv_to_rgb(hue, 0.55, 0.35 + 0.5 * y / 180)
        draw.line([(0, y), (320, y)], fill=(int(r * 255), int(g * 255), int(b * 255)))
    r, g, b = colorsys.hsv_to_rgb((hue + 0.12) % 1, 0.45, 0.95)
    draw.ellipse([100, 40, 220, 160], fill=(int(r * 255), int(g * 255), int(b * 255)))
    image.save(path, "JPEG", quality=80)


def main():
    root = Path(sys.argv[1]) / "PSP" / "GAME" / "WiliMini"
    (root / "cache").mkdir(parents=True, exist_ok=True)
    items = []
    for i, (title, author, duration) in enumerate(TITLES):
        bvid = f"BV1fixture{i:02d}"
        items.append({"bvid": bvid, "cid": 1000 + i, "title": title, "pic": "", "duration": duration,
                      "owner": {"name": author}, "stat": {"view": 12000 * (i + 1) + 345}})
        cover(root / "cache" / f"{bvid}.jpg", i / len(TITLES))
    (root / "cache" / "home.json").write_text(json.dumps({"code": 0, "data": {"list": items}}))
    local = [{"bvid": it["bvid"], "cid": it["cid"], "title": it["title"], "author": it["owner"]["name"],
              "duration": it["duration"], "played_at": 1700000000 + n, "position_ms": 60000}
             for n, it in enumerate(items[:3])]
    if "--many" in sys.argv:
        local = [dict(local[n % 3], bvid=items[n % len(items)]["bvid"], played_at=1700000000 + n,
                      title=f"{items[n % len(items)]['title']} #{n + 1}") for n in range(30)]
    (root / "history.json").write_text(json.dumps(local))
    (root / "later.json").write_text(json.dumps([dict(local[0], bvid=items[5]["bvid"], title=items[5]["title"])]))
    settings = {"version": 1, "dark_theme": "--dark" in sys.argv, "body_size": 14, "show_covers": True,
                "slow_repeat": False,
                "home_style": "metro" if "--metro" in sys.argv else "tiles" if "--tiles" in sys.argv else "classic"}
    if "--theme" in sys.argv:
        name = sys.argv[sys.argv.index("--theme") + 1]
        shutil.copytree(Path(__file__).resolve().parents[2] / "themes" / name, root / "themes" / name)
        settings["theme"] = name
    (root / "settings.json").write_text(json.dumps(settings))


main()
