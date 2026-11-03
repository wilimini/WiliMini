#!/usr/bin/env python3
"""Drive the built-in pinyin keyboard in PPSSPP and check what it draws.

Needs only PPSSPPHeadless and a built package (no account, network or ME firmware). The home
page's SQUARE button opens the keyboard (the built-in keyboard is the default input method). On a
machine with real network access (CI) the client is busy connecting for up to 20 emulated seconds
after start and ignores buttons, so the script waits START_AT seconds. The script types "nihao" with the d-pad by mirroring the keyboard's
navigation, then checks the screenshots. Pixel checks use the light theme's accent colour, so
they fail if the cursor is not on the key the script aimed for or no candidate is highlighted.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pngcheck import count_color, png_colors, png_rows  # noqa: E402

root = Path(__file__).resolve().parents[2]
out = Path(os.environ.get('BILI_IME_SMOKE_RESULTS', root / 'results/ime-smoke'))
out.mkdir(parents=True, exist_ok=True)
(root / 'build/headless').mkdir(parents=True, exist_ok=True)

UP, RIGHT, DOWN, LEFT = 0x10, 0x20, 0x40, 0x80
CROSS, SQUARE, START = 0x4000, 0x8000, 0x8
ACCENT = (204, 65, 113)  # light theme kPink

# Letter page rows as (label, width in tenths of a key); mirrors imeRows() in client/ui.cpp.
ROWS = [[(c, 10) for c in 'qwertyuiop'], [(c, 10) for c in 'asdfghjkl'],
        [('shift', 15)] + [(c, 10) for c in 'zxcvbnm'] + [('back', 15)],
        [('mode', 15), ('sym', 15), (',', 10), ('space', 30), ('.', 10), ('done', 20)]]


def key_rect(row, index):
    total = sum(u for _, u in row)
    before = sum(u for _, u in row[:index])
    start = 20 + (440 - total * 44 // 10) // 2
    x = start + before * 44 // 10
    return x, start + (before + row[index][1]) * 44 // 10 - 3


class Pad:
    def __init__(self, start):
        self.row = self.col = 0
        self.t = start
        self.script = []

    def press(self, mask, gap=0.4):
        self.script.append(f'{self.t:.2f}:0x{mask:x}:0.12')
        self.t += gap

    def goto(self, row, col):
        while self.row != row:
            step = 1 if row > self.row else -1
            x, right = key_rect(ROWS[self.row], self.col)
            centre = (x + right) // 2
            self.row += step
            self.col = min(range(len(ROWS[self.row])),
                           key=lambda i: abs((key_rect(ROWS[self.row], i)[0] + key_rect(ROWS[self.row], i)[1]) // 2 - centre))
            self.press(DOWN if step > 0 else UP)
        count = len(ROWS[self.row])
        while self.col != col:
            if (col - self.col) % count <= (self.col - col) % count:
                self.col = (self.col + 1) % count
                self.press(RIGHT)
            else:
                self.col = (self.col - 1) % count
                self.press(LEFT)

    def type(self, letter):
        for r, row in enumerate(ROWS):
            for c, (label, _) in enumerate(row):
                if label == letter:
                    self.goto(r, c)
                    self.press(CROSS)
                    return
        raise ValueError(letter)


# Startup connects Wi-Fi for up to 20 s (200 x 100 ms in network_connect) and a busy screen
# ignores buttons; presses at 15 s were lost on CI, where the emulator has a real network.
START_AT = 40.0
pad = Pad(START_AT)
pad.press(SQUARE, 2.0)
for letter in 'nihao':
    pad.type(letter)
pad.t += 0.8
shot_typed = pad.t
pad.press(SQUARE, 1.0)          # space commits the first candidate
shot_committed = pad.t
pad.press(START, 1.5)           # done: the search runs with the typed text
# 'before' and 'opened' are only diagnostics: they show what was on screen when the first button
# was pressed and just after, since CI screenshots cannot be opened from the authoring environment.
shots = {'before': START_AT - 0.5, 'opened': START_AT + 1.5, 'typed': shot_typed, 'committed': shot_committed, 'after': pad.t}

memstick = Path(tempfile.mkdtemp(prefix='ime-', dir=root / 'build/headless'))
subprocess.run([sys.executable, str(root / 'tools/ppsspp/seed-ui-fixture.py'), str(memstick)], check=True)
paths = {name: out / (name + '.png') for name in shots}
for path in paths.values():
    path.unlink(missing_ok=True)
env = dict(os.environ, BILI_HEADLESS_MEMSTICK=str(memstick), BILI_HEADLESS_PLAYER='ppsspp-sceMpeg',
           BILI_HEADLESS_TIMEOUT='240', BILI_HEADLESS_TIMEOUT_EMULATED=str(int(pad.t) + 1),
           BILI_HEADLESS_PRESS_SCRIPT=','.join(pad.script),
           BILI_HEADLESS_SCREENSHOT_AT=','.join(f'{shots[n]:.2f}:{paths[n]}' for n in shots))
with (out / 'run.log').open('w') as handle:
    subprocess.run(['sh', str(root / 'tools/ppsspp/run-client-headless.sh')], env=env,
                   stdout=handle, stderr=subprocess.STDOUT)
shutil.rmtree(memstick, ignore_errors=True)

failures = []


def check(name, condition, detail):
    print(f'{name}: {"ok" if condition else "FAILED"} ({detail})')
    if not condition:
        failures.append(name)


try:
    width, height, colors = png_colors(paths['typed'])
    check('typed screen drawn', (width, height) in ((480, 272), (512, 272)) and colors >= 12, f'{colors} colours')
    ox, oright = key_rect(ROWS[0], 'qwertyuiop'.index('o'))
    on_key = count_color(paths['typed'], ACCENT, (ox, 122, oright, 150))
    check('cursor on the "o" key', on_key > 0.7 * (oright - ox) * 28, f'{on_key} accent pixels')
    bar = count_color(paths['typed'], ACCENT, (20, 96, 460, 120))
    check('first candidate marked', bar >= 20, f'{bar} accent pixels in the candidate bar')
    # After space the composing text is gone: the candidate bar has no accent mark.
    check('candidate committed', count_color(paths['committed'], ACCENT, (20, 96, 460, 120)) == 0,
          'no candidate mark after space')
    width, height, colors = png_colors(paths['after'])
    check('keyboard closed', colors >= 12 and count_color(paths['after'], ACCENT, (20, 122, 460, 242)) < 200,
          'back on the search page')
except (OSError, AssertionError, KeyError, ValueError) as error:
    check('screenshots readable', False, str(error))


def sketch(path, columns=60, rows=17):
    """A coarse brightness picture of a screenshot (' .:-=+*#%@' from dark to light)."""
    width, height, pixels = png_rows(path)
    shades = ' .:-=+*#%@'
    lines = []
    for r in range(rows):
        line = ''
        for c in range(columns):
            x0, x1 = c * 480 // columns, (c + 1) * 480 // columns
            y0, y1 = r * 272 // rows, (r + 1) * 272 // rows
            total = count = 0
            for y in range(y0, y1):
                for x in range(x0, x1):
                    total += sum(pixels[y][x]) // 3
                    count += 1
            line += shades[min(9, total // count * 10 // 256)]
        lines.append(line)
    return '\n'.join(lines)


if failures:
    for name in ('before', 'opened', 'typed'):
        try:
            print(f'--- {name}.png at {shots[name]:.1f}s')
            print(sketch(paths[name]))
        except (OSError, AssertionError, KeyError, ValueError) as error:
            print(f'--- {name}.png unreadable: {error}')
    # CI cannot open the screenshots, so show the emulator log next to the verdict.
    try:
        print("--- last lines of", out / 'run.log')
        print('\n'.join((out / 'run.log').read_text(errors='replace').splitlines()[-60:]))
    except OSError:
        pass
    sys.exit('IME smoke failed: ' + ', '.join(failures))
print('IME smoke passed')
