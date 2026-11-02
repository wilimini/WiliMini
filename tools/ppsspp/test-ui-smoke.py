#!/usr/bin/env python3
"""Start the production client on a synthetic memstick in every home style and check the screenshots.

Needs only PPSSPPHeadless and a built package (no account, network or ME firmware).
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pngcheck import png_colors  # noqa: E402

root = Path(__file__).resolve().parents[2]
out = Path(os.environ.get('BILI_UI_SMOKE_RESULTS', root / 'results/ui-smoke'))
out.mkdir(parents=True, exist_ok=True)
(root / 'build/headless').mkdir(parents=True, exist_ok=True)

STYLES = {
    'classic': [],
    'tiles': ['--tiles'],
    'metro': ['--metro'],
    'metro-dark': ['--metro', '--dark'],
}
# Home hub, then open the first tile and its list; a crash or hang leaves a blank/black frame.
PRESS = '3.5:0x4000:0.15,6:0x2000:0.15'


failures = []
for name, flags in STYLES.items():
    memstick = Path(tempfile.mkdtemp(prefix='smoke-' + name + '-', dir=root / 'build/headless'))
    subprocess.run([sys.executable, str(root / 'tools/ppsspp/seed-ui-fixture.py'), str(memstick), *flags], check=True)
    shot = out / (name + '.png')
    shot.unlink(missing_ok=True)
    env = dict(os.environ, BILI_HEADLESS_MEMSTICK=str(memstick), BILI_HEADLESS_PLAYER='ppsspp-sceMpeg',
               BILI_HEADLESS_TIMEOUT='120', BILI_HEADLESS_PRESS_SCRIPT=PRESS,
               BILI_HEADLESS_SCREENSHOT_AT=f'8:{shot}')
    log = out / (name + '.log')
    with log.open('w') as handle:
        # The runner exits non-zero on its wall-clock timeout; the screenshot is the verdict.
        subprocess.run(['sh', str(root / 'tools/ppsspp/run-client-headless.sh')], env=env,
                       stdout=handle, stderr=subprocess.STDOUT)
    shutil.rmtree(memstick, ignore_errors=True)
    try:
        width, height, colors = png_colors(shot)
        ok = (width, height) in ((480, 272), (512, 272)) and colors >= 12
    except (OSError, AssertionError, KeyError, zlib.error) as error:
        width = height = colors = 0
        ok = False
        print(name, 'error:', error)
    print(f'{name}: {width}x{height}, {colors} colors -> {"ok" if ok else "FAILED"}')
    if not ok:
        failures.append(name)
if failures:
    sys.exit('UI smoke failed: ' + ', '.join(failures))
print('UI smoke passed')
